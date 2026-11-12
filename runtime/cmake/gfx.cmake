# SPDX-License-Identifier: GPL-3.0-or-later
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of the tools repository is compiled in as source (tools/tiling, tegra_tiling.h): found at
# ../tools or, from a worktree under runtime-wt/<name>, at ../../tools.
set(NXRT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools" CACHE PATH "The tools repository (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tiling/src/tegra_tiling.cpp")
    get_filename_component(_nxrt_td "${CMAKE_CURRENT_SOURCE_DIR}/../../tools" ABSOLUTE)
    set(NXRT_TOOLS_DIR "${_nxrt_td}" CACHE PATH "The tools repository (tools/tiling is used)" FORCE)
endif()
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

# ASTC -> BC7 transcoder (tools/textures): the prebuilt static libraries of the tools repository (scripts/build.ps1 there) and the
# decoder DLLs that astc2bc7 loads at run time (copied next to every executable that links nxrt_gfx).
set(NXRT_A2B_DIR "${NXRT_TOOLS_DIR}/textures/build/release" CACHE PATH "Build folder of tools/textures (astc2bc7.lib, bc7e_ispc.lib, astcdec-*.dll)")
add_library(nxrt_astc2bc7 INTERFACE)
target_include_directories(nxrt_astc2bc7 INTERFACE "${NXRT_TOOLS_DIR}/textures/include")
if(APPLE)
    # tools/textures is an MSVC + ISPC build (tools/textures/CMakeLists.txt pins the Windows ISPC), so the offline
    # ASTC -> BC7 transcoder does not exist for macOS yet. Until it does the runtime hands the guest's ASTC blocks to
    # Metal as they are (docs/macos-port.md, M2), which needs no transcoder; nothing links the library.
    message(STATUS "nxrt: ASTC -> BC7 transcoder is Windows-only; macOS uses ASTC passthrough (src/os/os_posix_a2b.cpp, docs/macos-port.md D4)")
elseif(NXRT_LINUX)
    # tools/textures is an MSVC + ISPC build too, and Vulkan has no ASTC sampling to fall back on: Linux decodes the blocks with
    # astcenc and encodes BC7 with bc7enc, both compiled from source into the runtime (src/os/os_posix_a2b.cpp). Nothing links
    # the tools library here either.
    message(STATUS "nxrt: ASTC -> BC7 transcoder: Linux uses astcenc + bc7enc (src/os/os_posix_a2b.cpp)")
else()
    if(NOT EXISTS "${NXRT_A2B_DIR}/astc2bc7.lib" OR NOT EXISTS "${NXRT_A2B_DIR}/bc7e_ispc.lib")
        message(FATAL_ERROR "astc2bc7.lib not found in ${NXRT_A2B_DIR}: build tools/textures first (pwsh tools/textures/scripts/build.ps1) or set NXRT_A2B_DIR")
    endif()
    target_link_libraries(nxrt_astc2bc7 INTERFACE "${NXRT_A2B_DIR}/astc2bc7.lib" "${NXRT_A2B_DIR}/bc7e_ispc.lib")
endif()
file(GLOB NXRT_A2B_DLLS "${NXRT_A2B_DIR}/astcdec-*-shared.dll")

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
# The vendor upscalers (src/gfx/extras): each one can be left out of the build. Off, its source is replaced by its *_off.cpp stub, which
# reports the backend unavailable; the callers then fall back as they do when the SDK is missing at run time (FSR 1 for NIS, no temporal pass
# for DLSS / FSR 3). The replay only sees the common interfaces (temporal.h, spatial_upscaler.h). FSR 1 is always built.
option(NXRT_UPSCALER_DLSS "NVIDIA DLSS / DLAA through Streamline (extras/temporal_dlss.cpp)" ON)
option(NXRT_UPSCALER_FSR3 "AMD FSR 3.1 through the FidelityFX API (extras/temporal_ffx.cpp)" ON)
option(NXRT_UPSCALER_NIS "NVIDIA Image Scaling (extras/nis_spatial.cpp)" ON)
if(NOT NXRT_WINDOWS)
    # The vendor SDKs ship Windows binaries only, and the DXGI/GDI HDR query is Win32: the stubs are used and the non-Windows HDR
    # query (extras/win_display_posix.cpp) takes its place. macOS gets MetalFX as a temporal backend later (docs/macos-port.md M2).
    set(NXRT_UPSCALER_DLSS OFF)
    set(NXRT_UPSCALER_FSR3 OFF)
    set(NXRT_UPSCALER_NIS OFF)
    list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "/src/gfx/extras/win_display\\.cpp$")
endif()
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "_off\\.cpp$")
set(_nxrt_up_src_DLSS temporal_dlss)
set(_nxrt_up_src_FSR3 temporal_ffx)
set(_nxrt_up_src_NIS nis_spatial)
foreach(_nxrt_up_opt IN ITEMS DLSS FSR3 NIS)
    set(_nxrt_up_src "${_nxrt_up_src_${_nxrt_up_opt}}")
    if(NOT NXRT_UPSCALER_${_nxrt_up_opt})
        list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "/extras/${_nxrt_up_src}\\.cpp$")
        list(APPEND NXRT_GFX_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/extras/${_nxrt_up_src}_off.cpp")
    endif()
endforeach()
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
# The About page of the F1 menu shows the commit this build was made from (read when CMake configures; absent without git).
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE NXRT_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _nxrt_git_rc)
    if(_nxrt_git_rc EQUAL 0 AND NXRT_BUILD_COMMIT)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/play/play_ui_about.cpp" PROPERTIES COMPILE_DEFINITIONS "NXRT_BUILD_COMMIT=\"${NXRT_BUILD_COMMIT}\"")
    endif()
endif()
target_include_directories(nxrt_gfx PUBLIC include src/gfx/nvn/third_party PRIVATE src/gfx src/gfx/nvn)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 nxrt_zstd SDL3::SDL3-static)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

add_executable(nxrt_gfx_demo apps/gfx_demo.cpp)
target_link_libraries(nxrt_gfx_demo PRIVATE nxrt_gfx)
nxrt_add_guest_segment(nxrt_gfx_demo)
if(NXRT_WINDOWS)
    target_link_options(nxrt_gfx_demo PRIVATE /MAP /DEBUG /STACK:8388608)
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_gfx_demo POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_demo>")
    endforeach()
elseif(APPLE)
    # volk dlopen()s "libvulkan.1.dylib"; shipping MoltenVK under that name next to the executable makes the port
    # self-contained (no Vulkan SDK needed on a player's Mac). @executable_path is the search path.
    add_custom_command(TARGET nxrt_gfx_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NXRT_MOLTENVK_LIBRARY}" "$<TARGET_FILE_DIR:nxrt_gfx_demo>/libvulkan.1.dylib")
endif()
