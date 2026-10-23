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

# libihs_godot_view: the ivi-homescreen platform-view producer that drives
# libgodot. Built out of tree against ivi-homescreen's libihs_shared, found as
#
#   1. an installed CMake package   (-DCMAKE_PREFIX_PATH=<ihs prefix>)
#   2. pkg-config                    (ivi-homescreen-shared.pc)
#   3. a configured shell build dir  (-DIHS_BUILD_DIR=<build>
#                                     -DIHS_SOURCE_DIR=<ivi-homescreen>)
#
# The plugin links nothing else: gbm, EGL and Vulkan are reached at run time
# (dlopen / the shell's own loader), and libgodot is dlopen'd by path.

cmake_minimum_required(VERSION 3.16)
project(ihs_godot_view LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(IHS_GODOT_VIEW_BUILD_TESTS "Build the unit tests" ON)

set(IHS_GODOT_BRIDGE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    CACHE PATH "Directory holding ihs_godot/bridge.h")

# --- ivi-homescreen ihs_shared ----------------------------------------------
find_package(ivi-homescreen-shared CONFIG QUIET)
if (TARGET ivi_homescreen::ihs_shared)
    set(IHS_SHARED_TARGET ivi_homescreen::ihs_shared)
    message(STATUS "ihs_shared: CMake package")
else ()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(IHS_SHARED QUIET IMPORTED_TARGET ivi-homescreen-shared)
    endif ()
    if (TARGET PkgConfig::IHS_SHARED)
        set(IHS_SHARED_TARGET PkgConfig::IHS_SHARED)
        message(STATUS "ihs_shared: pkg-config")
    elseif (IHS_BUILD_DIR AND IHS_SOURCE_DIR)
        set(_so "${IHS_BUILD_DIR}/shell/lib/libihs_shared.so")
        if (NOT EXISTS "${_so}")
            set(_so "${IHS_BUILD_DIR}/shared/libihs_shared.so")
        endif ()
        if (NOT EXISTS "${_so}")
            message(FATAL_ERROR "no libihs_shared.so in ${IHS_BUILD_DIR}")
        endif ()
        add_library(ihs_shared_import SHARED IMPORTED)
        set_target_properties(ihs_shared_import PROPERTIES
                IMPORTED_LOCATION "${_so}"
                INTERFACE_INCLUDE_DIRECTORIES
                "${IHS_SOURCE_DIR}/shared/include;${IHS_BUILD_DIR}/shared/include")
        set(IHS_SHARED_TARGET ihs_shared_import)
        message(STATUS "ihs_shared: ${_so}")
    else ()
        message(FATAL_ERROR
                "ivi-homescreen ihs_shared not found. Install it and set "
                "CMAKE_PREFIX_PATH, or pass -DIHS_BUILD_DIR and -DIHS_SOURCE_DIR.")
    endif ()
endif ()

# Vulkan headers only (no loader): the plugin calls Vulkan through the
# shell's interposed vkGetInstanceProcAddr.
find_path(VULKAN_HEADERS_DIR vulkan/vulkan.h)
if (NOT VULKAN_HEADERS_DIR)
    message(FATAL_ERROR "Vulkan headers not found (vulkan/vulkan.h)")
endif ()

set(CORE_SOURCES
        src/codec.cc
        src/input_map.cc
        src/log.cc
        src/surface_policy.cc
        src/view_params.cc
)

add_library(ihs_godot_view_core STATIC ${CORE_SOURCES})
target_include_directories(ihs_godot_view_core PUBLIC
        ${IHS_GODOT_BRIDGE_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${VULKAN_HEADERS_DIR})
# Headers only from ihs_shared here; the shared library links it.
target_include_directories(ihs_godot_view_core PUBLIC
        $<TARGET_PROPERTY:${IHS_SHARED_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(ihs_godot_view_core PUBLIC IHS_HAS_FORMAT_TO_N=1)

add_library(ihs_godot_view SHARED
        src/egl_image_importer.cc
        src/fence.cc
        src/gbm_allocator.cc
        src/godot_runtime.cc
        src/godot_view.cc
        src/ihs_log_sink.cc
        src/plugin.cc
        src/vk_image_pool.cc
)
target_link_libraries(ihs_godot_view PRIVATE
        ihs_godot_view_core
        ${IHS_SHARED_TARGET}
        ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(ihs_godot_view PRIVATE Threads::Threads)
target_compile_options(ihs_godot_view PRIVATE -Wall -Wextra)
target_link_options(ihs_godot_view PRIVATE -Wl,--no-undefined)
# libgodot.so is looked for beside the plugin first.
set_target_properties(ihs_godot_view PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")

include(GNUInstallDirs)
install(TARGETS ihs_godot_view LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/ihs_godot_view/ihs_godot_view.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ihs_godot_view)

if (IHS_GODOT_VIEW_BUILD_TESTS)
    enable_testing()
    add_executable(ihs_godot_view_tests test/tests.cc)
    target_link_libraries(ihs_godot_view_tests PRIVATE ihs_godot_view_core)
    add_test(NAME ihs_godot_view_tests COMMAND ihs_godot_view_tests)
endif ()
