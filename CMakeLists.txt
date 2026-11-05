# Copyright 2026 tocollective contributors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

configure_file(LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/bin/LICENSE" COPYONLY)
configure_file(THIRD_PARTY_LICENSES.txt "${CMAKE_CURRENT_SOURCE_DIR}/bin/THIRD_PARTY_LICENSES.txt" COPYONLY)

set(DEBUG_RUNTIME_SOURCES)
foreach(runtime_source IN LISTS MVM_RUNTIME_SOURCES)
    list(APPEND DEBUG_RUNTIME_SOURCES "${MVM_SOURCE_DIR}/${runtime_source}")
endforeach()
add_executable(mvm-debug source/debugger.c ${DEBUG_RUNTIME_SOURCES})
target_include_directories(mvm-debug PRIVATE
    "${MVM_SOURCE_DIR}/include" "${MVM_SOURCE_DIR}/source")
target_link_libraries(mvm-debug PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS}
    Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
set_target_properties(mvm-debug PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>")
if(MSVC)
    target_compile_options(mvm-debug PRIVATE /W4)
else()
    target_compile_options(mvm-debug PRIVATE -Wall -Wextra)
    target_link_libraries(mvm-debug PRIVATE m)
endif()
if(WIN32)
    target_link_libraries(mvm-debug PRIVATE ws2_32)
endif()
