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

# libgodot.so with the `ihs` engine module, built by Godot's own SCons from a
# CMake front: emb (toyota-connected/emb_cli) builds app modules with
# CMake/Meson/Cargo only, and hands them its cross toolchain through
# CMAKE_TOOLCHAIN_FILE. This project turns that toolchain into SCons
# arguments (CC/CXX/LINK, --sysroot, arch) and runs
#
#   scons platform=linuxbsd library_type=shared_library \
#         custom_modules=<this dir>/modules ...
#
# Standalone:
#   cmake -B build -S godot [-DGODOT_SOURCE_DIR=/path/to/godot]
#   cmake --build build          # -> build/libgodot.so
#
# Without GODOT_SOURCE_DIR, the pinned Godot revision below is fetched.

cmake_minimum_required(VERSION 3.20)
project(libgodot_ihs NONE)

set(GODOT_GIT_URL "https://github.com/godotengine/godot.git"
    CACHE STRING "Godot repository")
# The engine revision the ihs module is built and tested against (4.7-dev,
# DisplayServerEnums era). The module uses engine-internal C++ API, so move
# this deliberately.
set(GODOT_GIT_REF "2bbcb4960ed3a0efb12746f21660032b613f9c4c"
    CACHE STRING "Godot revision")
set(GODOT_SOURCE_DIR "" CACHE PATH "Existing Godot checkout (skips the fetch)")
set(GODOT_TARGET "template_release" CACHE STRING
    "SCons target: template_release | template_debug")
set(GODOT_LTO "none" CACHE STRING "SCons lto: none | auto | thin | full")
set(GODOT_SCONS_ARGS "" CACHE STRING "Extra SCons arguments (;-separated)")
set(GODOT_JOBS "" CACHE STRING "SCons -j (default: CPU count)")
option(GODOT_PATH_OVERRIDES
       "Allow --path in release builds (development; ship a .pck instead)" OFF)
option(GODOT_SDL "SDL3 input driver: gamepads read by Godot itself" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(SCONS_EXECUTABLE scons)
if (SCONS_EXECUTABLE)
    set(_scons "${SCONS_EXECUTABLE}")
else ()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import SCons"
                    RESULT_VARIABLE _no_scons OUTPUT_QUIET ERROR_QUIET)
    if (_no_scons)
        message(FATAL_ERROR "SCons not found (pip install scons, or the "
                            "distribution's scons package)")
    endif ()
    set(_scons "${Python3_EXECUTABLE}" -m SCons)
endif ()

# --- Godot source -----------------------------------------------------------
if (GODOT_SOURCE_DIR)
    set(_godot_src "${GODOT_SOURCE_DIR}")
else ()
    include(FetchContent)
    FetchContent_Declare(godot
            GIT_REPOSITORY "${GODOT_GIT_URL}"
            GIT_TAG "${GODOT_GIT_REF}"
            GIT_SHALLOW FALSE
            GIT_PROGRESS TRUE)
    FetchContent_GetProperties(godot)
    if (NOT godot_POPULATED)
        FetchContent_Populate(godot)
    endif ()
    set(_godot_src "${godot_SOURCE_DIR}")
endif ()
if (NOT EXISTS "${_godot_src}/SConstruct")
    message(FATAL_ERROR "no Godot source at ${_godot_src}")
endif ()

# --- toolchain -> SCons -----------------------------------------------------
set(_proc "${CMAKE_SYSTEM_PROCESSOR}")
if (NOT _proc)
    set(_proc "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif ()
string(TOLOWER "${_proc}" _proc)
if (_proc MATCHES "^(aarch64|arm64)$")
    set(_arch arm64)
elseif (_proc MATCHES "^(x86_64|amd64)$")
    set(_arch x86_64)
elseif (_proc MATCHES "^arm")
    set(_arch arm32)
elseif (_proc MATCHES "^riscv64")
    set(_arch rv64)
else ()
    message(FATAL_ERROR "unsupported processor '${_proc}'")
endif ()

# The compilers come from the toolchain file when emb passes one. A NONE
# project does not resolve them, so read the variables directly and fall back
# to the host compilers.
set(_args
        platform=linuxbsd
        arch=${_arch}
        target=${GODOT_TARGET}
        library_type=shared_library
        "custom_modules=${CMAKE_CURRENT_SOURCE_DIR}/modules"
        # Rendering goes through the ihs display driver; nothing here needs a
        # window system, and leaving them out keeps the sysroot small.
        x11=no
        wayland=no
        accesskit=no
        lto=${GODOT_LTO}
        debug_symbols=no
        # Godot's own copies of its libraries; system ones are dlopen'd.
        use_sowrap=yes)
if (NOT GODOT_SDL)
    list(APPEND _args sdl=no)
endif ()
if (GODOT_PATH_OVERRIDES)
    list(APPEND _args disable_path_overrides=no)
endif ()

set(_env "")
if (CMAKE_CROSSCOMPILING OR CMAKE_C_COMPILER OR CMAKE_CXX_COMPILER)
    if (CMAKE_C_COMPILER)
        list(APPEND _args "CC=${CMAKE_C_COMPILER}")
    endif ()
    if (CMAKE_CXX_COMPILER)
        list(APPEND _args "CXX=${CMAKE_CXX_COMPILER}" "LINK=${CMAKE_CXX_COMPILER}")
    endif ()
    set(_ccflags "")
    set(_linkflags "")
    if (CMAKE_SYSROOT)
        string(APPEND _ccflags " --sysroot=${CMAKE_SYSROOT}")
        string(APPEND _linkflags " --sysroot=${CMAKE_SYSROOT}")
    endif ()
    if (CMAKE_C_COMPILER_TARGET)
        string(APPEND _ccflags " --target=${CMAKE_C_COMPILER_TARGET}")
        string(APPEND _linkflags " --target=${CMAKE_C_COMPILER_TARGET}")
    endif ()
    # emb's tuning (-mcpu=...) arrives in the toolchain's init flags.
    string(APPEND _ccflags " ${CMAKE_C_FLAGS_INIT} ${CMAKE_C_FLAGS}")
    string(STRIP "${_ccflags}" _ccflags)
    string(STRIP "${_linkflags}" _linkflags)
    if (_ccflags)
        list(APPEND _args "ccflags=${_ccflags}")
    endif ()
    if (_linkflags)
        list(APPEND _args "linkflags=${_linkflags}")
    endif ()

    # SCons runs plain `ar`/`ranlib`; a host binutils cannot index foreign
    # objects. Put the toolchain's under those names, first on PATH.
    set(_shim "${CMAKE_CURRENT_BINARY_DIR}/toolshim")
    file(MAKE_DIRECTORY "${_shim}")
    foreach (_tool AR RANLIB)
        if (CMAKE_${_tool})
            string(TOLOWER "${_tool}" _name)
            file(CREATE_LINK "${CMAKE_${_tool}}" "${_shim}/${_name}" SYMBOLIC)
        endif ()
    endforeach ()
    list(APPEND _env "PATH=${_shim}:$ENV{PATH}")
    if (CMAKE_SYSROOT)
        list(APPEND _env
                "PKG_CONFIG_SYSROOT_DIR=${CMAKE_SYSROOT}"
                "PKG_CONFIG_LIBDIR=${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig")
    endif ()
endif ()

if (GODOT_JOBS)
    list(APPEND _args -j${GODOT_JOBS})
else ()
    cmake_host_system_information(RESULT _ncpu QUERY NUMBER_OF_LOGICAL_CORES)
    list(APPEND _args -j${_ncpu})
endif ()
list(APPEND _args ${GODOT_SCONS_ARGS})

set(_built "${_godot_src}/bin/libgodot.linuxbsd.${GODOT_TARGET}.${_arch}.so")
set(_out "${CMAKE_CURRENT_BINARY_DIR}/libgodot.so")
file(GLOB_RECURSE _module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include/ihs_godot/*.h")

string(REPLACE ";" " " _pretty "${_args}")
message(STATUS "libgodot: scons ${_pretty}")

add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${CMAKE_COMMAND} -E env ${_env} ${_scons} ${_args}
        COMMAND ${CMAKE_COMMAND} -E copy "${_built}" "${_out}"
        WORKING_DIRECTORY "${_godot_src}"
        DEPENDS ${_module_sources}
        COMMENT "Building libgodot (${_arch}, ${GODOT_TARGET}) with the ihs module"
        USES_TERMINAL
        VERBATIM)
add_custom_target(libgodot ALL DEPENDS "${_out}")

include(GNUInstallDirs)
install(FILES "${_out}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
