/*
 * Copyright 2026 tocollective contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Main-execution debug bridge. Control traffic uses an authenticated loopback
 * socket; program stdin/stdout/stderr stay independent of the protocol.
 * Requests: id TAB command [TAB arguments] LF. Responses/events: JSON LF. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "mvm/internal.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET debug_socket;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int debug_socket;
#define INVALID_SOCKET (-1)
#define CLOSE_SOCKET close
#endif

#define BUFFER_SIZE (2u * 1024u * 1024u)
#define INPUT_SIZE (128u * 1024u)
#define MAX_BREAKPOINTS 4096
#define PAGE_SIZE 1000
typedef struct writer { char *data; size_t used; bool failed; } writer;
typedef struct breakpoint { char *file; uint32_t line; } breakpoint;
typedef struct location { const char *file; uint32_t line, column; } location;
typedef struct debugger {
  mvm_t *vm;
  debug_socket socket;
  writer out;
  breakpoint breaks[MAX_BREAKPOINTS];
  size_t break_count, last_pc, last_depth, step_depth, stop_pc;
  location last_location;
  int mode; /* 0: continue, 1: into, 2: over, 3: out */
  bool running, done, skip_stop, faulted;
  char input[INPUT_SIZE];
  size_t input_used;
} debugger;

static void put(writer *w, const char *format, ...) {
  if (w->failed) return;
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(w->data + w->used, BUFFER_SIZE - w->used, format, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= BUFFER_SIZE - w->used) w->failed = true;
  else w->used += (size_t)n;
}
static void quoted(writer *w, const char *s) {
  put(w, "\"");
  if (s) for (size_t i = 0; s[i] && i < 16384; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') put(w, "\\%c", c);
    else if (c < 32) put(w, "\\u%04x", c);
    else put(w, "%c", c);
  }
  put(w, "\"");
}
static bool flush(debugger *d) {
  writer *w = &d->out;
  if (w->failed) {
    w->used = 0; w->failed = false;
    put(w, "{\"event\":\"error\",\"message\":\"Debugger response exceeds size limit\"}");
  }
  w->data[w->used++] = '\n';
  size_t sent = 0;
  while (sent < w->used) {
    int n = (int)send(d->socket, w->data + sent, (int)(w->used - sent), 0);
    if (n <= 0) { d->done = true; return false; }
    sent += (size_t)n;
  }
  w->used = 0;
  return true;
}
static location locate(mvm_function_type_t *f, size_t pc) {
  if (!f) return (location){0};
  size_t low = 0, high = f->debug_line_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (f->debug_lines[middle].pc <= pc) low = middle + 1;
    else high = middle;
  }
  if (!low) return (location){0};
  const mvm_debug_line_t *line = &f->debug_lines[low - 1];
  return (location){line->file, line->line, line->column};
}
static bool same_file(const char *a, const char *b) {
  if (!a || !b) return false;
  if (a == b || !strcmp(a, b)) return true;
#ifdef _WIN32
  char *left = _fullpath(NULL, a, 0), *right = _fullpath(NULL, b, 0);
  bool equal = left && right && !_stricmp(left, right);
#else
  char *left = realpath(a, NULL), *right = realpath(b, NULL);
  bool equal = left && right && !strcmp(left, right);
#endif
  free(left); free(right); return equal;
}
static bool changed(debugger *d, location l) {
  return !same_file(l.file, d->last_location.file) || l.line != d->last_location.line ||
         d->last_depth != d->vm->frame_count || d->vm->pc <= d->last_pc;
}
static void stopped(debugger *d, const char *reason, const char *message) {
  fflush(stdout);
  fflush(stderr);
  d->running = false; d->stop_pc = d->vm->pc;
  put(&d->out, "{\"event\":\"stopped\",\"reason\":"); quoted(&d->out, reason);
  if (message) { put(&d->out, ",\"description\":"); quoted(&d->out, message); }
  put(&d->out, "}"); flush(d);
}
static bool number(const char *s, size_t *value) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || n > (SIZE_MAX - (size_t)(*s - '0')) / 10) return false;
    n = n * 10 + (size_t)(*s - '0');
  }
  *value = n; return true;
}
static int unhex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static char *path_decode(const char *s) {
  if (!s || strlen(s) % 2 || strlen(s) > 32768) return NULL;
  const size_t n = strlen(s) / 2;
  char *path = malloc(n + 1);
  if (!path) return NULL;
  for (size_t i = 0; i < n; i++) {
    int a = unhex(s[2*i]), b = unhex(s[2*i+1]);
    if (a < 0 || b < 0 || !(a | b)) { free(path); return NULL; }
    path[i] = (char)(a * 16 + b);
  }
  path[n] = 0; return path;
}
static uint32_t breakpoint_line(debugger *d, const char *file, size_t requested) {
  uint32_t best = UINT32_MAX;
  for (size_t i = 0; i < d->vm->function_type_count; i++) {
    mvm_function_type_t *f = &d->vm->function_types[i];
    for (size_t j = 0; j < f->debug_line_count; j++) {
      mvm_debug_line_t *l = &f->debug_lines[j];
      if (same_file(file, l->file) && l->line >= requested && l->line < best) best = l->line;
    }
  }
  return best == UINT32_MAX ? 0 : best;
}
static void frame_json(debugger *d, size_t id, mvm_function_type_t *f, size_t pc) {
  location l = locate(f, pc);
  put(&d->out, "{\"id\":%zu,\"name\":", id);
  quoted(&d->out, f && f->name ? f->name : "<entry>");
  put(&d->out, ",\"line\":%u,\"column\":%u,\"pc\":%zu", l.line ? l.line : 1, l.column ? l.column : 1, pc);
  if (l.file) { put(&d->out, ",\"file\":"); quoted(&d->out, l.file); }
  put(&d->out, "}");
}
static const char *kind_name(mvm_kind_t kind) {
  static const char *const names[] = {
      "Void", "null", "Bool", "UByte", "UHalf", "UWord", "ULong", "Byte",
      "Half", "Word", "Long", "Float", "Double", "String", "Array", "Type",
      "Reference", "Function", "Thread", "Packet", "Union", "Mutex",
      "ConditionVariable", "Atomic"};
  return (unsigned)kind < sizeof(names) / sizeof(names[0]) ? names[kind]
                                                            : "Opaque";
}
static void append_text(char *buffer, size_t size, size_t *used,
                        const char *text) {
  if (*used >= size - 1) return;
  const int written = snprintf(buffer + *used, size - *used, "%s", text);
  if (written < 0) return;
  *used += (size_t)written < size - *used ? (size_t)written : size - *used - 1;
}
static void type_text(mvm_t *vm, uint32_t id, mvm_kind_t fallback, char *buffer,
                      size_t size, size_t *used, unsigned depth) {
  if (id >= vm->type_count || depth >= 32) {
    append_text(buffer, size, used, kind_name(fallback));
    return;
  }
  const mvm_type_t *type = &vm->types[id];
  if (type->name) {
    append_text(buffer, size, used, type->name);
    return;
  }
  if (type->tag == MVM_TYPE_PRIMITIVE) {
    append_text(buffer, size, used, kind_name(type->kind));
  } else if (type->tag == MVM_TYPE_AGGREGATE) {
    append_text(buffer, size, used, "Struct");
  } else if (type->tag == MVM_TYPE_ENUM) {
    type_text(vm, type->inner_type_id, type->kind, buffer, size, used, depth + 1);
  } else if (type->tag == MVM_TYPE_ARRAY) {
    if (type->flags & MVM_TYPE_FLAG_MUTABLE) append_text(buffer, size, used, "mut ");
    type_text(vm, type->inner_type_id, MVM_KIND_VOID, buffer, size, used, depth + 1);
    append_text(buffer, size, used, "[]");
  } else if (type->tag == MVM_TYPE_REFERENCE) {
    append_text(buffer, size, used,
                (type->flags & MVM_TYPE_FLAG_MUTABLE) ? "@mut " : "@");
    type_text(vm, type->inner_type_id, MVM_KIND_VOID, buffer, size, used, depth + 1);
  } else if (type->tag == MVM_TYPE_FUNCTION) {
    append_text(buffer, size, used, "(");
    for (size_t i = 0; i < type->argc; ++i) {
      if (i) append_text(buffer, size, used, ", ");
      type_text(vm, type->param_type_ids[i], MVM_KIND_VOID, buffer, size, used,
                depth + 1);
    }
    if (type->flags & MVM_TYPE_FLAG_VARIADIC)
      append_text(buffer, size, used, type->argc ? ", ..." : "...");
    append_text(buffer, size, used, "): ");
    type_text(vm, type->result_type_id, MVM_KIND_VOID, buffer, size, used,
              depth + 1);
  } else if (type->tag == MVM_TYPE_UNION) {
    append_text(buffer, size, used, "union(");
    for (size_t i = 0; i < type->variant_count; ++i) {
      if (i) append_text(buffer, size, used, ", ");
      type_text(vm, type->variant_type_ids[i], MVM_KIND_VOID, buffer, size,
                used, depth + 1);
    }
    append_text(buffer, size, used, ")");
  } else {
    append_text(buffer, size, used, kind_name(type->kind));
  }
}
static void value_json(writer *w, mvm_t *vm, mvm_value_t v, uint32_t type_id) {
  char type[512] = {0};
  size_t type_used = 0;
  type_text(vm, type_id, v.kind, type, sizeof(type), &type_used, 0);
  char value[128];
  switch (v.kind) {
  case MVM_KIND_VOID: snprintf(value, sizeof(value), "void"); break;
  case MVM_KIND_NULL: snprintf(value, sizeof(value), "null"); break;
  case MVM_KIND_BOOL: snprintf(value, sizeof(value), "%s", v.as.b ? "true" : "false"); break;
  case MVM_KIND_UBYTE: snprintf(value, sizeof(value), "%u", v.as.ub); break;
  case MVM_KIND_UHALF: snprintf(value, sizeof(value), "%u", v.as.uh); break;
  case MVM_KIND_UWORD: snprintf(value, sizeof(value), "%" PRIu32, v.as.uw); break;
  case MVM_KIND_ULONG: snprintf(value, sizeof(value), "%" PRIu64, v.as.ul); break;
  case MVM_KIND_SBYTE: snprintf(value, sizeof(value), "%d", v.as.sb); break;
  case MVM_KIND_SHALF: snprintf(value, sizeof(value), "%d", v.as.sh); break;
  case MVM_KIND_SWORD: snprintf(value, sizeof(value), "%" PRId32, v.as.sw); break;
  case MVM_KIND_SLONG: snprintf(value, sizeof(value), "%" PRId64, v.as.sl); break;
  case MVM_KIND_FLOAT: snprintf(value, sizeof(value), "%.9g", (double)v.as.f); break;
  case MVM_KIND_DOUBLE: snprintf(value, sizeof(value), "%.17g", v.as.d); break;
  case MVM_KIND_TYPE: snprintf(value, sizeof(value), "type#%u", v.as.type); break;
  case MVM_KIND_FUNC: snprintf(value, sizeof(value), "function@0x%X", v.as.func); break;
  case MVM_KIND_ARRAY:
    if (!v.as.array) snprintf(value, sizeof(value), "null");
    else snprintf(value, sizeof(value), "<Array length=%zu>", v.as.array->length);
    break;
  default:
    if ((v.kind == MVM_KIND_REFERENCE && !v.as.ptr) ||
        (v.kind == MVM_KIND_THREAD && !v.as.thread) ||
        (v.kind == MVM_KIND_UNION && !v.as.union_value) ||
        (v.kind == MVM_KIND_MUTEX && !v.as.mutex) ||
        (v.kind == MVM_KIND_CONDITION && !v.as.condition) ||
        (v.kind == MVM_KIND_ATOMIC && !v.as.atomic))
      snprintf(value, sizeof(value), "null");
    else snprintf(value, sizeof(value), "<%s>", kind_name(v.kind));
    break;
  }
  put(w, "{\"value\":");
  if (v.kind == MVM_KIND_STRING) quoted(w, v.as.s ? v.as.s : "null"); else quoted(w, value);
  put(w, ",\"type\":"); quoted(w, type);
  put(w, ",\"variablesReference\":0}");
}
static void error_response(debugger *d, size_t id, const char *message) {
  put(&d->out, "{\"id\":%zu,\"error\":", id); quoted(&d->out, message); put(&d->out, "}"); flush(d);
}
static void command(debugger *d, char *line) {
  char *parts[5] = {0};
  size_t count = 0;
  parts[count++] = line;
  for (char *p = line; *p; p++) if (*p == '\t') { *p = 0; if (count < 5) parts[count++] = p + 1; else return; }
  size_t id;
  if (count < 2 || !number(parts[0], &id)) { d->done = true; return; }
  const char *name = parts[1];
  if (!strcmp(name, "disconnect")) { put(&d->out, "{\"id\":%zu}", id); flush(d); d->done = true; return; }
  if (!strcmp(name, "pause")) {
    put(&d->out, "{\"id\":%zu}", id); flush(d);
    if (d->running) stopped(d, "pause", NULL);
    return;
  }
  if (!strcmp(name, "breakpoints")) {
    char *file = count > 2 ? path_decode(parts[2]) : NULL;
    if (!file) { error_response(d, id, "Invalid breakpoint source path"); return; }
    for (size_t i = 0; i < d->break_count;) {
      if (same_file(file, d->breaks[i].file)) { free(d->breaks[i].file); d->breaks[i] = d->breaks[--d->break_count]; } else i++;
    }
    put(&d->out, "{\"id\":%zu,\"breakpoints\":[", id);
    char *p = count > 3 ? parts[3] : NULL;
    bool comma = false;
    while (p && *p) {
      char *next = strchr(p, ','); if (next) *next++ = 0;
      size_t requested = 0;
      uint32_t actual = number(p, &requested) && requested ? breakpoint_line(d, file, requested) : 0;
      if (d->break_count == MAX_BREAKPOINTS) actual = 0;
      if (actual) {
        char *copy = malloc(strlen(file) + 1);
        if (!copy) actual = 0;
        else { strcpy(copy, file); d->breaks[d->break_count++] = (breakpoint){copy, actual}; }
      }
      put(&d->out, "%s{\"verified\":%s,\"line\":%zu}", comma ? "," : "", actual ? "true" : "false", actual ? (size_t)actual : requested);
      comma = true; p = next;
    }
    free(file); put(&d->out, "]}"); flush(d); return;
  }
  if (!strcmp(name, "continue") || !strcmp(name, "stepIn") || !strcmp(name, "next") || !strcmp(name, "stepOut") || !strcmp(name, "start")) {
    if (d->faulted) { error_response(d, id, "Execution cannot resume after a runtime error"); return; }
    if (d->running) { error_response(d, id, "Execution is already running"); return; }
    d->mode = !strcmp(name, "stepIn") ? 1 : !strcmp(name, "next") ? 2 : !strcmp(name, "stepOut") ? 3 : 0;
    d->step_depth = d->vm->frame_count;
    d->last_location = locate(d->vm->current_function, d->vm->pc);
    d->last_depth = d->vm->frame_count; d->last_pc = d->vm->pc;
    d->skip_stop = true; d->running = true;
    put(&d->out, "{\"id\":%zu}", id); flush(d);
    if (!strcmp(name, "start")) {
      d->skip_stop = false; d->last_location = (location){0};
      if (count > 2 && !strcmp(parts[2], "1")) stopped(d, "entry", NULL);
    }
    return;
  }
  if (d->running) { error_response(d, id, "Pause execution before inspecting state"); return; }
  if (!strcmp(name, "stack")) {
    put(&d->out, "{\"id\":%zu,\"frames\":[", id);
    frame_json(d, 0, d->vm->current_function, d->stop_pc);
    size_t count_frames = d->vm->frame_count;
    for (size_t i = 0; i < count_frames && i < PAGE_SIZE - 1; i++) {
      mvm_frame_t *f = &d->vm->frames[count_frames - 1 - i];
      put(&d->out, ","); frame_json(d, i + 1, f->caller_function, f->call_pc);
    }
    put(&d->out, "]}"); flush(d); return;
  }
  if (!strcmp(name, "variables")) {
    size_t frame = 0, start = 0, page_count = PAGE_SIZE;
    if (count < 3 || (strcmp(parts[2], "global") && !number(parts[2], &frame)) || (count > 3 && !number(parts[3], &start)) || (count > 4 && !number(parts[4], &page_count))) { error_response(d, id, "Invalid variable reference"); return; }
    const bool globals = !strcmp(parts[2], "global");
    if (!globals && frame > d->vm->frame_count) { error_response(d, id, "Invalid stack frame"); return; }
    size_t base = 0, end = d->vm->stack->size;
    mvm_function_type_t *function = NULL;
    if (!globals) {
      const size_t depth = d->vm->frame_count - frame;
      base = depth ? d->vm->frames[depth - 1].base : 0;
      if (frame) {
        end = d->vm->frames[depth].base;
        function = d->vm->frames[depth].caller_function;
      } else function = d->vm->current_function;
    } else end = d->vm->global_count;
    put(&d->out, "{\"id\":%zu,\"variables\":[", id);
    bool comma = false;
    page_count = page_count && page_count < PAGE_SIZE ? page_count : PAGE_SIZE;
    mvm_runtime_lock(d->vm);
    if (!globals && function && function->debug_variable_count) {
      size_t visible = 0, emitted = 0;
      for (size_t i = 0; i < function->debug_variable_count && emitted < page_count; ++i) {
        const mvm_debug_variable_t *variable = &function->debug_variables[i];
        if (variable->slot >= end - base) continue;
        if (visible++ < start) continue;
        put(&d->out, "%s{\"name\":", comma ? "," : "");
        quoted(&d->out, variable->name);
        put(&d->out, ",\"data\":");
        value_json(&d->out, d->vm, d->vm->stack->data[base + variable->slot],
                   variable->type_id);
        put(&d->out, "}");
        comma = true;
        ++emitted;
      }
    } else {
      for (size_t i = 0; base <= end && start < end - base && i < page_count && i < end - base - start; i++) {
        const size_t slot = start + i;
        char fallback_name[64];
        const char *variable_name = fallback_name;
        uint32_t type_id = UINT32_MAX;
        if (globals && d->vm->debug_globals) {
          variable_name = d->vm->debug_globals[slot].name;
          type_id = d->vm->debug_globals[slot].type_id;
        } else {
          snprintf(fallback_name, sizeof(fallback_name), "%s[%zu]",
                   globals ? "global" : "slot", slot);
        }
        put(&d->out, "%s{\"name\":", comma ? "," : "");
        quoted(&d->out, variable_name);
        put(&d->out, ",\"data\":");
        value_json(&d->out, d->vm,
                   globals ? d->vm->globals[slot] : d->vm->stack->data[base + slot],
                   type_id);
        put(&d->out, "}"); comma = true;
      }
    }
    mvm_runtime_unlock(d->vm);
    put(&d->out, "]}"); flush(d); return;
  }
  error_response(d, id, "Unsupported debugger command");
}
static bool receive_commands(debugger *d, bool wait) {
  fd_set read_set; FD_ZERO(&read_set); FD_SET(d->socket, &read_set);
  struct timeval timeout = {0};
  int ready = select((int)d->socket + 1, &read_set, NULL, NULL, wait ? NULL : &timeout);
  if (ready < 0) {
#ifndef _WIN32
    if (errno == EINTR) return true;
#endif
    return false;
  }
  if (!ready) return true;
  int n = (int)recv(d->socket, d->input + d->input_used, (int)(INPUT_SIZE - 1 - d->input_used), 0);
  if (n <= 0) return false;
  d->input_used += (size_t)n; d->input[d->input_used] = 0;
  size_t used = 0;
  for (size_t i = 0; i < d->input_used; i++) if (d->input[i] == '\n') {
    d->input[i] = 0; command(d, d->input + used); used = i + 1;
    if (d->done) return true;
  }
  memmove(d->input, d->input + used, d->input_used - used); d->input_used -= used;
  return d->input_used < INPUT_SIZE - 1;
}
static void execute_one(debugger *d) {
  mvm_t *vm = d->vm;
  location l = locate(vm->current_function, vm->pc);
  if (!d->skip_stop && l.file && changed(d, l)) {
    bool hit = false;
    for (size_t i = 0; i < d->break_count; i++) if (vm->frame_count >= d->last_depth && d->breaks[i].line == l.line && same_file(d->breaks[i].file, l.file)) { hit = true; break; }
    if (hit) { stopped(d, "breakpoint", NULL); return; }
    if (d->mode == 1 || (d->mode == 2 && vm->frame_count <= d->step_depth) || (d->mode == 3 && vm->frame_count < d->step_depth)) { stopped(d, "step", NULL); return; }
  }
  d->skip_stop = false; d->last_location = l; d->last_depth = vm->frame_count; d->last_pc = vm->pc;
  mvm_result_t result = mvm_update(vm);
  if (result == MVM_RESULT_BREAKPOINT) {
    stopped(d, "breakpoint", NULL); d->stop_pc = d->last_pc;
  } else if (result != MVM_RESULT_OK) {
    d->faulted = true;
    char description[128]; snprintf(description, sizeof(description), "mvm runtime error %d at pc 0x%zX", result, d->last_pc);
    stopped(d, "exception", description); d->stop_pc = d->last_pc;
  } else if (!vm->working) {
    int code = vm->stack->size && vm->stack->data[vm->stack->size - 1].kind == MVM_KIND_SWORD ? vm->stack->data[vm->stack->size - 1].as.sw : 1;
    put(&d->out, "{\"event\":\"exited\",\"exitCode\":%d}", code); flush(d); d->done = true;
  }
}
int main(int argc, char **argv) {
  size_t port = 0;
  if (argc < 4 || !number(argv[1], &port) || !port || port > 65535 || strlen(argv[2]) != 64) {
    fputs("usage: mvm-debug <loopback-port> <token> <program.mvm> [arguments...]\n", stderr); return 1;
  }
#ifdef _WIN32
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
#else
  signal(SIGPIPE, SIG_IGN);
#endif
  debugger *d = calloc(1, sizeof(*d));
  if (!d) return 1;
  d->out.data = malloc(BUFFER_SIZE);
  if (!d->out.data) { free(d); return 1; }
  d->socket = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET; address.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  int status = 1;
  if (d->socket == INVALID_SOCKET || connect(d->socket, (struct sockaddr *)&address, sizeof(address))) {
#ifdef _WIN32
    fprintf(stderr, "Cannot connect to debug adapter: socket error %d\n", WSAGetLastError());
#else
    fprintf(stderr, "Cannot connect to debug adapter: %s\n", strerror(errno));
#endif
    goto cleanup;
  }
  d->vm = mvm_create(argv[3], MVM_MEMORY_DEFAULT_MIN, MVM_MEMORY_DEFAULT_MAX, MVM_FRAME_DEFAULT_LIMIT);
  if (!d->vm) { fputs("Failed to load debug image\n", stderr); goto cleanup; }
  d->stop_pc = d->vm->pc;
  if (d->vm->entry_argc == 1) {
    mvm_value_t values;
    size_t n = (size_t)argc - 3;
    if (mvm_array_new(d->vm, MVM_KIND_STRING, n, &values) != MVM_RESULT_OK || !mvm_stack_push(d->vm->stack, values)) goto cleanup;
    for (size_t i = 0; i < n; i++) if (mvm_array_set(d->vm, values, i, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = argv[3 + i]}) != MVM_RESULT_OK) goto cleanup;
  }
  put(&d->out, "{\"event\":\"ready\",\"token\":"); quoted(&d->out, argv[2]); put(&d->out, "}");
  if (!flush(d)) goto cleanup;
  size_t instructions = 0;
  while (!d->done) {
    if (!d->running || !(instructions++ % 256)) if (!receive_commands(d, !d->running)) break;
    if (d->running && !d->done) execute_one(d);
  }
  status = 0;
cleanup:
  if (d->vm) mvm_destroy(d->vm);
  for (size_t i = 0; i < d->break_count; i++) free(d->breaks[i].file);
  if (d->socket != INVALID_SOCKET) CLOSE_SOCKET(d->socket);
  free(d->out.data); free(d);
#ifdef _WIN32
  WSACleanup();
#endif
  return status;
}
