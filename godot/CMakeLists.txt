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

# The engine revision the ihs module is built and tested against (master,
# 4.7/4.8-dev, DisplayServerEnums era). The module uses engine-internal C++
# API, so move this deliberately.
set(GODOT_GIT_REF "2bbcb4960ed3a0efb12746f21660032b613f9c4c"
    CACHE STRING "Godot revision")
set(GODOT_GIT_URL "https://github.com/godotengine/godot.git"
    CACHE STRING "Godot repository")
set(GODOT_SOURCE_DIR "" CACHE PATH "Existing Godot checkout (skips the fetch)")
set(GODOT_TARGET "template_release" CACHE STRING
    "SCons target: template_release | template_debug")
set(GODOT_LTO "none" CACHE STRING "SCons lto: none | auto | thin | full")
set(GODOT_SCONS_ARGS "" CACHE STRING "Extra SCons arguments (;-separated)")
set(GODOT_JOBS "" CACHE STRING "SCons -j (default: CPU count)")
# Release templates without path overrides refuse --main-pack as well as
# --path, and the producer hands Godot its .pck with --main-pack.
option(GODOT_PATH_OVERRIDES
       "Allow --main-pack and --path in release builds (needed to run a .pck)" ON)
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
    # Fetch just the pinned commit (depth 1): FetchContent cannot shallow
    # clone a SHA, and Godot's full history is gigabytes.
    find_package(Git REQUIRED)
    set(_godot_src "${CMAKE_CURRENT_BINARY_DIR}/godot-src")
    set(_stamp "${_godot_src}/.ihs-godot-ref")
    set(_have "")
    if (EXISTS "${_stamp}")
        file(READ "${_stamp}" _have)
    endif ()
    if (NOT _have STREQUAL GODOT_GIT_REF)
        message(STATUS "Fetching Godot ${GODOT_GIT_REF}")
        file(REMOVE_RECURSE "${_godot_src}")
        file(MAKE_DIRECTORY "${_godot_src}")
        foreach (_cmd
                "init;-q"
                "remote;add;origin;${GODOT_GIT_URL}"
                "fetch;--depth;1;origin;${GODOT_GIT_REF}"
                "checkout;-q;FETCH_HEAD")
            execute_process(COMMAND "${GIT_EXECUTABLE}" ${_cmd}
                    WORKING_DIRECTORY "${_godot_src}"
                    RESULT_VARIABLE _rc)
            if (_rc)
                message(FATAL_ERROR "git ${_cmd} failed (${_rc})")
            endif ()
        endforeach ()
        file(WRITE "${_stamp}" "${GODOT_GIT_REF}")
    endif ()
endif ()
if (NOT EXISTS "${_godot_src}/SConstruct")
    message(FATAL_ERROR "no Godot source at ${_godot_src}")
endif ()

# Engine changes the module needs (patches/*.patch, in order). The fetched
# checkout is this project's: it is reset and patched on every configure, so
# an edited patch replaces the old one. In a GODOT_SOURCE_DIR, a patch that
# already applies in reverse is in place; one that applies neither way is an
# error, since the module would not build.
find_package(Git REQUIRED)
file(GLOB _patches "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(SORT _patches)
if (NOT GODOT_SOURCE_DIR)
    execute_process(COMMAND "${GIT_EXECUTABLE}" checkout -q -- .
            WORKING_DIRECTORY "${_godot_src}"
            RESULT_VARIABLE _rc)
    if (_rc)
        message(FATAL_ERROR "could not reset ${_godot_src}")
    endif ()
endif ()
foreach (_patch ${_patches})
    get_filename_component(_name "${_patch}" NAME)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
            WORKING_DIRECTORY "${_godot_src}"
            RESULT_VARIABLE _not_applied OUTPUT_QUIET ERROR_QUIET)
    if (NOT _not_applied)
        continue ()
    endif ()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
            WORKING_DIRECTORY "${_godot_src}"
            RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if (_rc)
        message(FATAL_ERROR "patch ${_name} does not apply to ${_godot_src}:\n${_err}")
    endif ()
    message(STATUS "Applied ${_name}")
endforeach ()

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
    # CMake links with the compile flags too, and emb's carry the multiarch
    # -B/-L that find crti.o and libc in a Debian sysroot. Then the
    # toolchain's own link flags (-fuse-ld=lld, ...).
    string(APPEND _linkflags " ${CMAKE_CXX_FLAGS_INIT} ${CMAKE_CXX_FLAGS}"
           " ${CMAKE_SHARED_LINKER_FLAGS_INIT} ${CMAKE_SHARED_LINKER_FLAGS}")
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

# SCons switches to use_llvm for a clang CXX, and names the output .llvm.so.
set(_llvm "")
if (CMAKE_CXX_COMPILER MATCHES "clang[^/]*$")
    set(_llvm ".llvm")
endif ()
set(_built "${_godot_src}/bin/libgodot.linuxbsd.${GODOT_TARGET}.${_arch}${_llvm}.so")
set(_out "${CMAKE_CURRENT_BINARY_DIR}/libgodot.so")
file(GLOB_RECURSE _module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch"
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
