<!--
Copyright 2026 tocollective contributors
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# mvm-debug

Native debugger for the mvm virtual machine. It runs an M program inside the VM
and exposes execution control to the [M Language VS Code extension](../mvm-vscode-plugin/README.md).
The extension provides the Debug Adapter Protocol (DAP) interface; `mvm-debug`
communicates with it through a separate loopback TCP connection.

## Build

Keep `mvm-debug/`, `mvm/`, `mvm-c/`, and `mvm-fmt/` beside each other in the common
M directory. The debugger is built through the mvm CMake project and uses its
runtime sources and bundled ZIP and TOML libraries.

Requirements: a C11/C17-capable C compiler, CMake 3.16+, pkg-config, libffi,
Flex, Bison, and platform thread support.

Run from the common M directory:

```sh
cmake -S mvm -B mvm/build -DCMAKE_BUILD_TYPE=Debug
cmake --build mvm/build --target mvm-c mvm-debug --config Debug
```

The debugger executable is written to `mvm-debug/bin/mvm-debug`
(`mvm-debug/bin/mvm-debug.exe` on Windows), including with multi-configuration
generators. Intermediate files remain in `mvm/build/`. The compiler is written
to `mvm-c/bin/mvm-c`.

## Debug in VS Code

Compile programs with `mvm-c --debug` (or `mvm-c -d`) to include source locations
and variable metadata:

```sh
mvm-c/bin/mvm-c --debug -I mvm-lib mvm-c/examples/hello.m -o mvm/build/hello.mvm
```

Install the M Language extension, open the common M directory, and press F5
with an M source file open. The extension finds `mvm-debug/bin/mvm-debug`
automatically and compiles source files with debug information before launch.
It can also launch an existing `.mvm` image.

For a separate workspace, set the executable paths in `.vscode/settings.json`:

```json
{
  "m.compilerPath": "/path/to/M/mvm-c/bin/mvm-c",
  "m.debuggerPath": "/path/to/M/mvm-debug/bin/mvm-debug",
  "m.includePaths": ["/path/to/M/mvm-lib"]
}
```

Supported operations include source breakpoints, stop on entry, continue, pause,
step into/over/out, stack inspection, local and global values, and stops on
`breakpoint()` or VM runtime errors. Names and declared types are shown when
debug metadata is available; otherwise values use `slot[n]` and `global[n]` names.

## Adapter interface

The executable is intended to be started by a debug adapter:

```text
mvm-debug <loopback-port> <token> <program.mvm> [arguments...]
```

The adapter listens on `127.0.0.1`, selects a port, and supplies a 64-character
session token. The debugger connects to that port and sends a JSON `ready`
event containing the token for the adapter to validate. Program arguments
follow the image path. Standard input, output, and error belong to the M program
and are independent of the control connection.

Requests are UTF-8 lines with tab-separated fields:
`id<TAB>command[<TAB>argument...]<LF>`. Responses and events are newline-delimited
JSON. Responses carry the request `id`; events carry an `event` name.
Commands include `start`, `breakpoints`, `continue`, `pause`, `stepIn`, `next`,
`stepOut`, `stack`, `variables`, and `disconnect`. See
[`source/debugger.c`](source/debugger.c) and the extension's
[`transport.ts`](../mvm-vscode-plugin/src/transport.ts) for the protocol implementation.

## Limitations and checks

- Execution control and stack inspection cover the main execution only;
  worker OS threads continue running.
- Pause is processed between VM instructions; a blocked native call must return
  before execution can pause.
- Complex objects are displayed as summaries. Editing values, evaluating M
  expressions, conditional breakpoints, logpoints, and attaching are unsupported.
- A breakpoint on a nonexecutable line moves to the next executable line in
  that file, or remains unverified if there is none.
- Limits include 4096 breakpoints, 1000 values per page, 1000 stack frames,
  and 16 KiB per string.

Run the extension's integration suite against the built tools (requires Node.js,
npm, and local TCP sockets):

```sh
cmake --build mvm/build --target mvm-c mvm-debug mvm-fmt --config Debug
cd mvm-vscode-plugin
npm ci
npm run test:integration
```

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

Bundled third-party license texts are included in
[THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt). The build copies both license
files alongside the executable in `bin/`.
