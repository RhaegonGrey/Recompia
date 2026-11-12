# SPDX-License-Identifier: GPL-3.0-or-later
# pkp.exe: the runtime (../runtime) + the four lifted game libraries (built by ../recomp/scripts/build_game.cmd pkp into ../work/build/pkp).
#   pwsh scripts\build.ps1            configure + build into game-build\build\release
# Nothing derived from the game lives here: the libraries are imported from the work folder, the NSOs are read at run time.
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(WIN32 AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(pkp_game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PKP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(PKP_LIB_DIR "${PKP_ROOT}/work/build/pkp" CACHE PATH "folder with pkp_main.lib pkp_sdk.lib pkp_subsdk0.lib pkp_rtld.lib (built by recomp scripts build_game.cmd pkp)")
set(PKP_EXEFS_DEFAULT "${PKP_ROOT}/game/dump/exefs" CACHE PATH "default ExeFS folder baked into pkp.exe (--exefs / PKP_EXEFS override it)")

# Release packages (PKP_RELEASE=ON): pkp.exe is a Windows GUI program (double click opens the game window only, no console; errors show in
# a message box, output still reaches a terminal it was started from), looks for ./exefs by default, and carries no local build paths
# (source paths in assert strings, the .pdb path): see the installed-layout comment in src/pkp_main.cpp.
# __FILE__ (the assert strings of the audio renderer) never carries the build folder, in any build: paths are written relative to the checkout.
if(WIN32)
    add_compile_options("-clang:-fmacro-prefix-map=${PKP_ROOT}=.")
else()
    add_compile_options("-ffile-prefix-map=${PKP_ROOT}=.")   # Apple clang spells the same flag without the MSVC pass-through
endif()
option(PKP_RELEASE "build pkp.exe as a release package binary" OFF)
if(PKP_RELEASE)
    set(PKP_GUI_SUBSYSTEM ON)
    set(PKP_EXEFS_DEFAULT "exefs")
    if(WIN32)
        add_compile_options("-clang:-ffile-prefix-map=${PKP_ROOT}=.")
    endif()
endif()

# The release version ("0.1.0"; "dev" for local builds). The game log, --version and the exe's file properties show it.
# game-build/scripts/build.ps1 -Version sets it.
set(PKP_VERSION "dev" CACHE STRING "release version shown by pkp.exe")
set(PKP_VER_MAJOR 0)
set(PKP_VER_MINOR 0)
set(PKP_VER_PATCH 0)
if(PKP_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)")
    set(PKP_VER_MAJOR ${CMAKE_MATCH_1})
    set(PKP_VER_MINOR ${CMAKE_MATCH_2})
    set(PKP_VER_PATCH ${CMAKE_MATCH_3})
endif()
# PKP_RUNTIME_DIR: which runtime checkout compiles in (default the main one; a worktree such as runtime-wt/boot2 for branch work)
set(PKP_RUNTIME_DIR "${PKP_ROOT}/runtime" CACHE PATH "runtime source tree compiled into pkp.exe")
set(NXRT_GAMES "pkp" CACHE STRING "" FORCE)            # runtime/games/pkp only
set(NXRT_RECOMP_INCLUDE "${PKP_ROOT}/recomp/include" CACHE PATH "")
if(PKP_RELEASE)
    set(NXRT_TESTTOOLS OFF)                             # release packages carry no test tooling (runtime option, default on)
    set(PKP_GAMEPLAY_DEFAULTS_CHECK ON)                 # and every gameplay mod on by default unless opt_in (runtime/games/pkp/core/pkp_settings_table.h)
endif()
# Release model (b) (PKP_RT_DLL=ON, docs/RELEASE.md): the runtime becomes a shared library (pkp_rt.dll, libpkp_rt.dylib,
# libpkp_rt.so) that a game executable links against. Declared before the runtime below, because the static libraries it links have to
# be compiled position independent. The block after the module list says what each platform builds from it.
option(PKP_RT_DLL "build the runtime as a shared library for release model (b): pkp_rt.dll / libpkp_rt.dylib / libpkp_rt.so" OFF)
if(PKP_RT_DLL AND NOT WIN32)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
add_subdirectory("${PKP_RUNTIME_DIR}" runtime)

# The module list (names, bases, load order) is recomp/games/pkp/config.py: the imported libraries below and the header
# src/pkp_main.cpp includes come from it (the player build writes the same header for src/pkp_stub.cpp).
include("${PKP_ROOT}/recomp/cmake/NxRecomp.cmake")
nxrecomp_build_info(pkp PKP_MOD)
nxrecomp_module_header(pkp "${CMAKE_CURRENT_BINARY_DIR}/gen/nx_modules.gen.h")
set(PKP_GEN_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(PKP_MODULE_LIBS "")
foreach(m ${PKP_MOD_MODULES})
    list(APPEND PKP_MODULE_LIBS pkp_${m})
    add_library(pkp_${m} STATIC IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(pkp_${m} PROPERTIES IMPORTED_LOCATION "${PKP_LIB_DIR}/pkp_${m}.lib")
    else()
        set_target_properties(pkp_${m} PROPERTIES IMPORTED_LOCATION "${PKP_LIB_DIR}/libpkp_${m}.a")
    endif()
endforeach()

# Release model (b) (PKP_RT_DLL=ON, docs/RELEASE.md): no game code in anything built here. The runtime and the boot code become
# Release model (b): the runtime as a shared library (pkp_rt.dll, libpkp_rt.dylib, libpkp_rt.so) that a game executable built
# separately links against. On Windows pkp.exe is linked on the player's PC from the libraries lifted out of their own dump and
# src/pkp_stub.cpp (tools/build/pkp_build.py in the package, with the bundled Zig); on macOS and Linux the same pair is built here,
# because the exe needs the Mach-O __GUEST segment / the lift already ran. Nothing lifted is imported into the runtime library itself.
if(PKP_RT_DLL AND NOT WIN32 AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "PKP_RT_DLL is the Windows, macOS and Linux release model (b)")
endif()
if(PKP_RT_DLL)
    if(WIN32)
        add_library(pkp_rt SHARED src/pkp_main.cpp src/pkp_rt.def)
    else()
        # A Mach-O dylib exports its global symbols by default, so pkp_rt.def has no counterpart here; ELF exports nothing it is
        # not told to, so Linux gets a version script made from the same list (below).
        add_library(pkp_rt SHARED src/pkp_main.cpp)
        if(APPLE)
            set_target_properties(pkp_rt PROPERTIES OUTPUT_NAME pkp_rt INSTALL_NAME_DIR "@rpath")
            # The dylib counterpart of pkp_rt.def: exporting a symbol also makes the linker include the static object that defines
            # it, which is how the lifted code's imports (call_indirect, host_import, stx_diag, ...) reach the dylib.
            target_link_options(pkp_rt PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/pkp_rt.exports")
        else()
            set_target_properties(pkp_rt PROPERTIES OUTPUT_NAME pkp_rt)
            # The same list as an ELF version script: `global:` is pkp_rt.def's EXPORTS, `local: *` hides the rest (the C++ runtime
            # symbols a shared object would otherwise publish). --no-undefined would be wrong here: the lifted code's imports are
            # resolved from the executable, exactly as on Windows.
            file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/pkp_rt.exports" _pkp_rt_exports REGEX "^_[A-Za-z]")
            set(_pkp_rt_global "")
            set(_pkp_rt_keep "")
            foreach(_sym ${_pkp_rt_exports})
                string(REGEX REPLACE "^_" "" _sym "${_sym}")
                string(APPEND _pkp_rt_global "    ${_sym};\n")
                # An ELF version script only filters what is exported; unlike a Mach-O export list it does not pull the defining
                # archive member in. Nothing inside the runtime calls these (the lifted code does), so ask for them explicitly.
                list(APPEND _pkp_rt_keep "-Wl,--undefined=${_sym}")
            endforeach()
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pkp_rt.map" "PKP_RT_1 {\n  global:\n${_pkp_rt_global}  local: *;\n};\n")
            target_link_options(pkp_rt PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/pkp_rt.map" ${_pkp_rt_keep})
        endif()
    endif()
    set_source_files_properties(src/pkp_main.cpp PROPERTIES COMPILE_DEFINITIONS "PKP_VERSION=\"${PKP_VERSION}\"")
    target_compile_definitions(pkp_rt PRIVATE PKP_RT_DLL=1 PKP_GUI_SUBSYSTEM=1 PKP_EXEFS_DEFAULT="exefs")
    target_include_directories(pkp_rt PRIVATE "${PKP_GEN_INCLUDE}")
    target_link_libraries(pkp_rt PRIVATE nxrt_game_pkp nxrt_gfx nxrt)
    if(WIN32)
        # /DEBUG off: no .pdb path in the DLL. Low host allocations as for pkp.exe (guest ranges at 0x71_0000_0000 stay free).
        target_link_options(pkp_rt PRIVATE /HIGHENTROPYVA:NO /DEBUG:NONE /Brepro)
    endif()
    target_compile_options(pkp_rt PRIVATE "-ffile-prefix-map=${PKP_ROOT}=.")
    add_custom_command(TARGET pkp_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PKP_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:pkp_rt>/fonts/DejaVuSans.ttf")
    if(WIN32)   # astcdec-*-shared.dll is the Windows decoder; macOS samples ASTC in Metal, Linux decodes with the static astcenc
        file(GLOB PKP_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
        foreach(dll ${PKP_A2B_DLLS})
            add_custom_command(TARGET pkp_rt POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:pkp_rt>")
        endforeach()
    endif()
    if(APPLE)
        # The game executable of model (b): src/pkp_stub.cpp and the lifted modules, linked against the dylib. It carries the
        # __GUEST segment (the main executable's header is what reserves the range before libmalloc runs), and finds the dylib
        # next to itself.
        add_executable(pkp src/pkp_stub.cpp)
        set_source_files_properties(src/pkp_stub.cpp PROPERTIES COMPILE_DEFINITIONS "PKP_VERSION=\"${PKP_VERSION}\"")
        target_include_directories(pkp PRIVATE "${PKP_GEN_INCLUDE}")
        target_link_libraries(pkp PRIVATE ${PKP_MODULE_LIBS} pkp_rt)
        nxrt_add_guest_segment(pkp)
        set_target_properties(pkp PROPERTIES BUILD_RPATH "@executable_path" INSTALL_RPATH "@executable_path")
        add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PKP_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:pkp>/fonts/DejaVuSans.ttf")
        add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pkp>/prompts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PKP_RUNTIME_DIR}/assets/prompts/prompt_icons.pak" "$<TARGET_FILE_DIR:pkp>/prompts/prompt_icons.pak")
        add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NXRT_MOLTENVK_LIBRARY}" "$<TARGET_FILE_DIR:pkp>/libvulkan.1.dylib")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The same pair on Linux: src/pkp_stub.cpp and the lifted modules, linked against libpkp_rt.so, which the executable finds
        # next to itself ($ORIGIN). No __GUEST segment: the guest ranges are reserved by the runtime (mmap MAP_FIXED_NOREPLACE), and
        # no Vulkan runtime is copied in (the system loader and ICDs are used).
        add_executable(pkp src/pkp_stub.cpp)
        set_source_files_properties(src/pkp_stub.cpp PROPERTIES COMPILE_DEFINITIONS "PKP_VERSION=\"${PKP_VERSION}\"")
        target_include_directories(pkp PRIVATE "${PKP_GEN_INCLUDE}")
        target_link_libraries(pkp PRIVATE ${PKP_MODULE_LIBS} pkp_rt)
        set_target_properties(pkp PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
        # The main thread's stack: lifted code nests deeply (the Windows /STACK:134217728 equivalent). glibc ignores this PT_GNU_STACK
        # size for the main thread; the runtime raises RLIMIT_STACK to 128 MiB before main (os_posix.cpp raise_main_stack).
        target_link_options(pkp PRIVATE "-Wl,-z,stack-size=134217728")
        add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PKP_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:pkp>/fonts/DejaVuSans.ttf")
        add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pkp>/prompts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PKP_RUNTIME_DIR}/assets/prompts/prompt_icons.pak" "$<TARGET_FILE_DIR:pkp>/prompts/prompt_icons.pak")
    endif()
    return()
endif()

if(WIN32)
    add_executable(pkp src/pkp_main.cpp res/pkp.rc)
else()
    add_executable(pkp src/pkp_main.cpp)   # res/pkp.rc is the Windows resource script (version info, manifest)
endif()
set_source_files_properties(src/pkp_main.cpp PROPERTIES COMPILE_DEFINITIONS "PKP_VERSION=\"${PKP_VERSION}\"")
if(WIN32)
    set_source_files_properties(res/pkp.rc PROPERTIES COMPILE_DEFINITIONS "PKP_VER_MAJOR=${PKP_VER_MAJOR};PKP_VER_MINOR=${PKP_VER_MINOR};PKP_VER_PATCH=${PKP_VER_PATCH};PKP_VER_TEXT=\"${PKP_VERSION}\""
                                OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/pkp.manifest")
endif()
target_compile_definitions(pkp PRIVATE PKP_EXEFS_DEFAULT="${PKP_EXEFS_DEFAULT}")
target_include_directories(pkp PRIVATE "${PKP_GEN_INCLUDE}")
target_link_libraries(pkp PRIVATE ${PKP_MODULE_LIBS} nxrt_game_pkp nxrt_gfx nxrt)   # nxrt_game_pkp: runtime/games/pkp
nxrt_add_guest_segment(pkp)   # macOS: reserve the guest address space in the Mach-O header (runtime/cmake/guest_segment.cmake)
if(APPLE)
    # volk opens libvulkan.1.dylib next to the executable (src/gfx/vk_device.cpp): MoltenVK, as the demo target also gets it.
    add_custom_command(TARGET pkp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NXRT_MOLTENVK_LIBRARY}" "$<TARGET_FILE_DIR:pkp>/libvulkan.1.dylib")
endif()
# the imported libraries must be lifted with this tree's site lists (a lift from another checkout or an older main.sites links fine but
# its missing hooks never run): checked on every build
nxrecomp_check_lift(pkp_lift_check PKP_MOD "${PKP_LIB_DIR}" pkp_)
add_dependencies(pkp pkp_lift_check)
# A linker map next to the exe (host frames get names), a big main stack (lifted
# code nests deeply), low host allocations so the guest ranges at 0x71_0000_0000 and up stay free.
if(WIN32)
    if(PKP_RELEASE)
        target_compile_definitions(pkp PRIVATE PKP_GUI_SUBSYSTEM=1)
        target_link_options(pkp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /PDBALTPATH:pkp.pdb)
    endif()
    target_link_options(pkp PRIVATE /MAP /STACK:134217728 /HIGHENTROPYVA:NO /DEBUG /MANIFEST:NO)   # res/pkp.rc carries the manifest
elseif(APPLE)
    # The main thread's stack: lifted code nests deeply. /STACK has no direct equivalent; -stack_size writes the size into the
    # Mach-O LC_MAIN header, which is what the kernel gives the main thread (128 MiB, the same number as the Windows flag).
    target_link_options(pkp PRIVATE "-Wl,-stack_size,0x8000000")
else()
    # Linux: glibc ignores the PT_GNU_STACK size for the main thread; the runtime raises RLIMIT_STACK to 128 MiB before main
    # (os_posix.cpp raise_main_stack). The size is still written for a loader that reads it.
    target_link_options(pkp PRIVATE "-Wl,-z,stack-size=134217728")
endif()

# The ASTC decoder DLLs astc2bc7 loads at run time (NVN textures in ASTC are transcoded to BC7 on first use): next to pkp.exe.
# (NXRT_A2B_DIR is the tools/textures build folder the runtime links its transcoder from: cmake/gfx.cmake)
# The shared-font fallback (runtime src/services/pl.cpp looks in fonts\ next to the exe): without any font the game crashes at boot.
add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${PKP_RUNTIME_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:pkp>/fonts/DejaVuSans.ttf")

# The prompt icon pack (runtime src/gfx/play/prompts.cpp: 598 36x36 RGBA icons, xbox/ps/steam/deck/kbm): the button prompts
# replace the game's icon textures with these. Without it the game's own icons stay ("no icon pack (prompt_icons.pak) found").
add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pkp>/prompts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${PKP_RUNTIME_DIR}/assets/prompts/prompt_icons.pak" "$<TARGET_FILE_DIR:pkp>/prompts/prompt_icons.pak")

# Keyboard and mouse prompt icons (runtime games/pkp/input/pkp_kbm.cpp reads assets\kbm_icons\<name>.png next to the exe): Kenney's Input Prompts
# (CC0), the "Keyboard & Mouse/Default" PNGs, kept in the work folder (not in any repository). Without them the prompts show key names as text.
set(PKP_KBM_ICONS_DIR "${PKP_ROOT}/work/assets/kenney_input_prompts/Keyboard & Mouse/Default" CACHE PATH "Kenney Input Prompts PNG folder for assets/kbm_icons")
if(EXISTS "${PKP_KBM_ICONS_DIR}")
    # a script copies them: the folder name has an '&', which the build's command lines (cmd) cannot carry
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/copy_kbm_icons.cmake"
        "file(GLOB _png \"${PKP_KBM_ICONS_DIR}/*.png\")
file(COPY \${_png} DESTINATION \"\${DST}\")
")
    add_custom_target(pkp_kbm_icons COMMAND ${CMAKE_COMMAND} "-DDST=$<TARGET_FILE_DIR:pkp>/assets/kbm_icons" -P "${CMAKE_CURRENT_BINARY_DIR}/copy_kbm_icons.cmake" VERBATIM)
    add_dependencies(pkp pkp_kbm_icons)
endif()

file(GLOB PKP_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")
foreach(dll ${PKP_A2B_DLLS})
    add_custom_command(TARGET pkp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:pkp>")
endforeach()
