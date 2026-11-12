# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt: game-independent runtime (OS/HLE layer, graphics shell) for static recompilations of Switch games.
# Build: scripts\build.ps1   (clang-cl + Ninja, Release)  |  macOS: cmake --preset macos-arm64 (docs/macos-port.md)
cmake_minimum_required(VERSION 3.25)

if(WIN32)
    # Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
    # included, uses it; the libraries this one links must agree (a mismatch is a link error).
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()
project(nxrt C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Native targets: Windows x64 (the shipped build), Apple Silicon macOS (docs/macos-port.md) and Linux x86-64
# (docs/linux-port.md). Everything else is unsupported.
if(WIN32)
    set(NXRT_WINDOWS TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # x86-64 only, for the same reason the Windows build is: the guest is AArch64 and the recompiler emits the host's
    # x86 translation path, where a 128-bit atomic is cmpxchg16b (-mcx16). An arm64 Linux build would need the arm64
    # host path macOS has and is not a target.
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "nxrt builds for Linux x86-64 only (CMAKE_SYSTEM_PROCESSOR is '${CMAKE_SYSTEM_PROCESSOR}')")
    endif()
    set(NXRT_LINUX TRUE)
elseif(APPLE)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR "nxrt builds for Apple Silicon (arm64) only: the guest is AArch64 and the recompiler emits AArch64-derived C++ "
                            "with 64-bit atomics; an x86_64 macOS build would need the x86 translation path the project does not have")
    endif()
    set(NXRT_APPLE TRUE)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS floor: MoltenVK 1.4 needs 12.0, C++20 std::format and the SDL3 macOS "
                                                           "backend are comfortable at 13")
    endif()
else()
    message(FATAL_ERROR "nxrt targets native Windows x64, Apple Silicon macOS and Linux x86-64 only")
endif()

# The generated-code ABI header (Ctx, call_indirect, ...) lives in the recompiler (recomp/include/nxrecomp/).
# Found next to the checkout (../recomp) or, for a git worktree under runtime-wt/<name>, two levels up (../../recomp).
set(NXRT_RECOMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/include" CACHE PATH "Directory that holds nxrecomp/")
if(NOT EXISTS "${NXRT_RECOMP_INCLUDE}/nxrecomp/ctx.h" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include/nxrecomp/ctx.h")
    get_filename_component(_nxrt_ri "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/include" ABSOLUTE)
    set(NXRT_RECOMP_INCLUDE "${_nxrt_ri}" CACHE PATH "Directory that holds nxrecomp/" FORCE)
endif()

if(NXRT_WINDOWS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(/W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                            -Wno-cast-function-type-strict -Wno-language-extension-token)
    endif()
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
else()
    # The POSIX shim (src/os/nx_os.h) stands in for the Win32 API. -Wno-deprecated-declarations covers the shim's deliberate use of
    # pthread_* primitives; everything else stays at the compiler's default warnings.
    add_compile_options(-Wno-unused-parameter -Wno-missing-field-initializers -Wno-deprecated-declarations)
    # glibc hides POSIX and BSD declarations behind feature-test macros (clock_gettime, CLOCK_MONOTONIC, IFF_UP, strdup, ...), and
    # -std=c11 / -std=c++20 with extensions off define __STRICT_ANSI__, which turns all of them off. _GNU_SOURCE turns them on for
    # the whole tree, C and C++ alike (third_party/pokopia-coop is strict C11 and needs it most); macOS ignores the macro.
    add_compile_definitions(NXRT_POSIX=1 _GNU_SOURCE=1)
endif()

option(NXRT_BUILD_GFX "Build the graphics shell (SDL3 + Vulkan + NVN front end, src/gfx, cmake/gfx.cmake)" ON)
# Game modules (games/<id>/, docs/GAME_BOUNDARY.md): the title-specific code a game exe links.
option(NXRT_BUILD_GAMES "Build the game modules in games/" ON)
set(NXRT_GAMES "pkp" CACHE STRING "ids of the game modules to build (games/<id>/), a list")
# Test tooling (src/testtools, include/nxrt/tt.h: deterministic mode, scripted runs, traces, input record and replay). Off: the runtime
# builds without it and src/core/tt_off.cpp answers its entry points (everything off). Release packages turn it off (game-build: PKP_RELEASE).
# Code outside the runtime library tests the compile definition NXRT_TESTTOOLS (0 or 1).
option(NXRT_TESTTOOLS "Build the test tooling (src/testtools)" ON)

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/gfx/")   # the graphics shell is its own library (cmake/gfx.cmake)
if(NXRT_TESTTOOLS)
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/core/tt_off[.]cpp$")
else()
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/testtools/")
endif()
if(NXRT_WINDOWS)
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/os/os_posix.*\\.cpp$")   # the shim itself: POSIX targets only
    # Apple-only and Linux-only platform services: Windows plays through XAudio2 and lists devices through WASAPI (below).
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_coreaudio\\.cpp$")
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_devices_posix\\.cpp$")
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_sdl3\\.cpp$")
else()
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/os/os_win32\\.cpp$")
    # Subsystems built on a Windows-only SDK. Each has a POSIX counterpart on the milestone list in
    # docs/macos-port.md: Media Foundation movies (AVFoundation) and the XAudio2 sink (CoreAudio) are M3.
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/media/movie_mf\\.cpp$")
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_xaudio2\\.cpp$")
    list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_devices\\.cpp$")   # WASAPI device enumeration
    if(NOT APPLE)
        list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_coreaudio\\.cpp$")    # the CoreAudio sink is macOS only
        list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_devices_posix\\.cpp$")   # CoreAudio device list
    endif()
    if(NOT NXRT_LINUX)
        list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/services/audio_sdl3\\.cpp$")   # the SDL3 sink (Linux: ALSA/PipeWire/PulseAudio)
    endif()
    # album.cpp (caps:su) stays: its PNG writer has an ImageIO branch on macOS and a zlib one (third_party/miniz) on Linux.
endif()
add_library(nxrt STATIC ${NXRT_SOURCES})
target_include_directories(nxrt PUBLIC include)
if(NOT NXRT_WINDOWS)
    # The signal handler's thread-locals (t_guard, t_thread_name) must be a plain thread-pointer access: with the default
    # global-dynamic model the handler calls __tls_get_addr, which is not async-signal-safe, and the guarded-call machinery
    # then reads a null guard and lets a caught fault become fatal (M3, 2026-11-10). Static TLS: the variable is in the
    # executable's own block. macOS: local-exec (the runtime is linked into the executable or into libpkp_rt.dylib, whose
    # TLS block is fixed at load). Linux: initial-exec, which a shared object may use and which resolves before main.
    if(APPLE)
        set(_nxrt_tls_model "-ftls-model=local-exec")
    else()
        set(_nxrt_tls_model "-ftls-model=initial-exec")
    endif()
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/os/os_posix.cpp"
                                PROPERTIES COMPILE_OPTIONS "${_nxrt_tls_model}")
endif()

if(NOT NXRT_WINDOWS)
    # `<windows.h>` and the other Win32 SDK headers resolve to src/os/compat/, which forward to the shim. Only on the non-Windows target,
    # so the Windows build keeps using the real SDK headers byte for byte.
    target_include_directories(nxrt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/os" "${CMAKE_CURRENT_SOURCE_DIR}/src/os/compat")
    # astc2bc7's interface header (tools/textures). The transcoder itself is an x86 ISPC build, so the POSIX build uses
    # src/os/os_posix_a2b.cpp: the same helpers, and an honest "unsupported" for the kernels Metal replaces (D4).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/textures/include/astc2bc7.h")
        target_include_directories(nxrt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/textures/include")
    endif()
    if(APPLE)
        target_link_libraries(nxrt PUBLIC "-framework CoreFoundation" "-framework IOKit" "-framework Security" "-framework ImageIO"
                                          "-framework CoreGraphics"
                                          # the CoreAudio sink (services/audio_coreaudio.cpp) and its device list
                                          "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio")
    else()
        # Linux: dlopen/dlsym (plugins, the Vulkan loader) and pthreads. Both live in libc on a current glibc; naming them
        # keeps older glibc and other libc implementations working.
        target_link_libraries(nxrt PUBLIC ${CMAKE_DL_LIBS} pthread)
    endif()
    # The BC7 encoder the texture patch needs (src/gfx/nvn/nvn_tex_patch.cpp) on the platforms without astc2bc7's ISPC kernels:
    # third_party/bc7enc (MIT), driven by os_posix_a2b.cpp.
    add_library(nxrt_bc7enc STATIC third_party/bc7enc/bc7enc.c)
    target_include_directories(nxrt_bc7enc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bc7enc")
    set_target_properties(nxrt_bc7enc PROPERTIES C_STANDARD 11)
    target_link_libraries(nxrt PRIVATE nxrt_bc7enc)
    # ASTC decoding. Metal samples ASTC natively, so macOS never needs a decoder; Vulkan on Linux does (no desktop GPU exposes
    # ASTC), and the game's textures are ASTC. ARM astcenc (Apache-2.0, vendored for tools/textures) decompresses to RGBA8, which
    # a2b_encode_bc7 then compresses with bc7enc above. Decompressor-only build: a transcode never re-encodes ASTC.
    set(NXRT_ASTCENC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/textures/third_party/astc-encoder")
    if(NXRT_LINUX AND EXISTS "${NXRT_ASTCENC_DIR}/CMakeLists.txt")
        set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
        set(ASTCENC_DECOMPRESSOR ON CACHE BOOL "" FORCE)
        set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
        set(ASTCENC_ISA_SSE2 OFF CACHE BOOL "" FORCE)
        set(ASTCENC_ISA_AVX2 OFF CACHE BOOL "" FORCE)
        set(ASTCENC_ISA_NONE OFF CACHE BOOL "" FORCE)
        set(ASTCENC_ISA_NATIVE OFF CACHE BOOL "" FORCE)
        set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
        set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
        set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
        add_subdirectory("${NXRT_ASTCENC_DIR}" "${CMAKE_BINARY_DIR}/astcenc" EXCLUDE_FROM_ALL)
        target_link_libraries(nxrt PRIVATE astcdec-sse4.1-static)
        target_compile_definitions(nxrt PRIVATE NXRT_HAVE_ASTCENC=1)
    endif()
endif()
if(EXISTS "${NXRT_RECOMP_INCLUDE}")
    target_include_directories(nxrt PUBLIC "${NXRT_RECOMP_INCLUDE}")
endif()
if(NXRT_WINDOWS)
    target_link_libraries(nxrt PUBLIC psapi)
endif()
add_subdirectory(third_party/pokopia-coop)   # [net] protocol = native: pokopia-native's Link Play (MIT, C11)
target_link_libraries(nxrt PRIVATE pokopia_coop)
include(cmake/guest_segment.cmake)   # macOS: reserve the guest address space in the executable's Mach-O header
add_subdirectory(third_party/miniz)   # zlib streams (MIT): the Pokopia save repair, games/pkp/core/pkp_save_uuid.cpp
if(NXRT_LINUX)
    target_link_libraries(nxrt PRIVATE nxrt_miniz)   # album.cpp's PNG writer (services/album.cpp)
endif()
add_subdirectory(third_party/modtopia)   # the Modtopia mod loader core (MIT, C11): the mods host, src/mods/ (nxrt/mods.h)
target_link_libraries(nxrt PUBLIC nxrt_modtopia PRIVATE nxrt_modtopia_mappings)
include(cmake/media_deps.cmake)   # libvpx for the Switch 2 video decoders (optional)

if(NXRT_BUILD_GFX)
    include(cmake/gfx.cmake)
endif()
if(NXRT_LINUX AND TARGET SDL3::SDL3-static)
    # The Linux audio sink and output-device list (src/services/audio_sdl3.cpp) go through SDL3, which dlopen()s ALSA,
    # PulseAudio or PipeWire at run time: no audio headers to install, no extra package at run time, and the same library
    # the graphics shell already links. Without the graphics shell there is no SDL3, and Linux falls back to the null sink.
    target_link_libraries(nxrt PRIVATE SDL3::SDL3-static)
    target_compile_definitions(nxrt PRIVATE NXRT_HAVE_SDL3_AUDIO=1)
endif()

if(NXRT_BUILD_GAMES)
    foreach(_game ${NXRT_GAMES})
        add_subdirectory(games/${_game})
    endforeach()
endif()
# The launcher's branding and factory settings: the data part of one game module and its launcher.rc (version info, manifest). Without that
# module the launcher builds unbranded, with no resources. The launcher is Win32 UI (cmake/launcher.cmake), so it is Windows only.
if(NXRT_WINDOWS AND TARGET mpr_launcher AND TARGET nxrt_game_${NXRT_LAUNCHER_GAME}_data)
    target_sources(mpr_launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/${NXRT_LAUNCHER_GAME}/launcher.rc")
    target_link_libraries(mpr_launcher PRIVATE nxrt_game_${NXRT_LAUNCHER_GAME}_data)
    target_compile_definitions(mpr_launcher PRIVATE NXRT_LAUNCHER_GAME_REGISTER=nxrt_register_game_${NXRT_LAUNCHER_GAME}_data)
    target_link_options(mpr_launcher PRIVATE /MANIFEST:NO)   # launcher.rc carries the manifest
endif()


# Developer tool, in no package: the controls.ini docs (docs/CONFIG.md) and the launcher's schema (docs/controls_schema.json) written from
# the settings table with one game's factory defaults (its data library, docs/GAME_BOUNDARY.md). scripts/gen_settings_doc.ps1 runs it.
if(NXRT_BUILD_GFX AND NXRT_BUILD_GAMES)
    foreach(_game ${NXRT_GAMES})
        if(TARGET nxrt_game_${_game}_data)
            add_executable(nxrt_settings_doc_${_game} EXCLUDE_FROM_ALL apps/settings_doc.cpp)
            target_link_libraries(nxrt_settings_doc_${_game} PRIVATE nxrt_gfx nxrt_game_${_game}_data)
            target_compile_definitions(nxrt_settings_doc_${_game} PRIVATE NXRT_SETTINGS_DOC_REGISTER=nxrt_register_game_${_game}_data)
        endif()
    endforeach()
endif()
