# Copyright 2026 Joel Winarske
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# End-to-end smoke tests. They need no GPU and no ivi-homescreen: Mesa's
# lavapipe (Vulkan) and llvmpipe (EGL) render, and pv_host_smoke installs a
# mock shell through libihs_shared's host seam.
#
#   cmake -B build-test -S test -DCMAKE_PREFIX_PATH=<ihs_shared prefix> \
#         -DLIBGODOT=<libgodot.so built with disable_path_overrides=no> \
#         -DIHS_GODOT_VIEW=<libihs_godot_view.so>
#   cmake --build build-test && ctest --test-dir build-test --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(ihs_godot_view_smoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBGODOT "" CACHE FILEPATH "libgodot.so with the ihs module")
set(IHS_GODOT_VIEW "" CACHE FILEPATH "libihs_godot_view.so")

find_package(ivi-homescreen-shared CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

set(_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(_project "${CMAKE_CURRENT_SOURCE_DIR}/bridge_smoke/project")

add_executable(bridge_smoke bridge_smoke/bridge_smoke.cc)
target_include_directories(bridge_smoke PRIVATE ${_root}/include)
target_link_libraries(bridge_smoke PRIVATE ${CMAKE_DL_LIBS})

add_executable(pv_host_smoke pv_host_smoke/pv_host_smoke.cc
        ${_root}/plugin/src/codec.cc)
target_include_directories(pv_host_smoke PRIVATE ${_root}/include)
target_link_libraries(pv_host_smoke PRIVATE ivi_homescreen::ihs_shared
        Vulkan::Vulkan Threads::Threads ${CMAKE_DL_LIBS})

enable_testing()
if (LIBGODOT)
    foreach (driver vulkan opengl3 opengl3_es)
        add_test(NAME bridge_${driver}
                COMMAND bridge_smoke ${LIBGODOT} ${_project} ${driver})
    endforeach ()
    if (IHS_GODOT_VIEW)
        foreach (driver vulkan opengl3 opengl3_es)
            add_test(NAME pv_shm_${driver}
                    COMMAND pv_host_smoke ${IHS_GODOT_VIEW} ${LIBGODOT}
                    ${_project} ${driver})
        endforeach ()
        add_test(NAME pv_vk_image
                COMMAND pv_host_smoke ${IHS_GODOT_VIEW} ${LIBGODOT}
                ${_project} vulkan vk_image)
        add_test(NAME pv_vk_image_gl_falls_back
                COMMAND pv_host_smoke ${IHS_GODOT_VIEW} ${LIBGODOT}
                ${_project} opengl3 vk_image)
    endif ()
endif ()
