# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned third-party sources for the graphics shell (S4b). Every archive is fetched by URL and checked against a
# SHA256. The archives are cached in NXRT_DEPS_DIR (shared by checkouts and worktrees, survives a clean build); the
# extracted sources and the SDL build live in the build directory. Licences and pins are listed in THIRD_PARTY_NOTICES.md and README.md.
include(FetchContent)
if(APPLE)
    set(_nxrt_default_deps "$ENV{HOME}/Library/Caches/nxrt/deps")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # XDG: ~/.cache/nxrt/deps unless XDG_CACHE_HOME says otherwise.
    if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_nxrt_default_deps "$ENV{XDG_CACHE_HOME}/nxrt/deps")
    else()
        set(_nxrt_default_deps "$ENV{HOME}/.cache/nxrt/deps")
    endif()
else()
    set(_nxrt_default_deps "$ENV{LOCALAPPDATA}/nxrt/deps")
endif()
set(NXRT_DEPS_DIR "${_nxrt_default_deps}" CACHE PATH "Where the pinned third-party sources are downloaded to")
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)   # extracted files take the time of extraction

# SDL 3.4.18 (zlib licence), released 2026-10-02. Built from source as a static library.
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
# Vulkan-Headers vulkan-sdk-1.4.363.0 (Apache-2.0 OR MIT)
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
    SOURCE_SUBDIR none)
# volk vulkan-sdk-1.4.363.0 (MIT)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d
    SOURCE_SUBDIR none)
# VulkanMemoryAllocator v3.4.0 (MIT)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
    SOURCE_SUBDIR none)
# Dear ImGui v1.92.9b (MIT)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8
    SOURCE_SUBDIR none)

# zstd: the shader preload pack (src/gfx/nvn/nvn_shaders.cpp). The library sources only (no CLI, no x86-64 assembly).
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR none)

# SDL: static, only what a game needs.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
set(SDL_VENDOR_INFO "nxrt" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(sdl3 vulkan_headers volk vma imgui zstd)

file(GLOB NXRT_ZSTD_SOURCES "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c" "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(nxrt_zstd STATIC ${NXRT_ZSTD_SOURCES})
target_include_directories(nxrt_zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(nxrt_zstd PRIVATE ZSTD_DISABLE_ASM)
set_target_properties(nxrt_zstd PROPERTIES C_STANDARD 11)

# volk (loader) next to the Vulkan headers; the Vulkan headers come from the pin, not from the installed SDK.
add_library(nxrt_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(nxrt_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
if(APPLE)
    # MoltenVK: no VK_USE_PLATFORM_WIN32_KHR, and the surface comes from VK_EXT_metal_surface through SDL.
    target_compile_definitions(nxrt_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_METAL_EXT)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The surface comes from SDL, but the extensions it reports have to be types volk knows. X11 is always there (Xwayland
    # counts); Wayland only when its headers are installed, which keeps the build free of a libwayland-dev requirement.
    set(_nxrt_vk_platforms VK_USE_PLATFORM_XLIB_KHR VK_USE_PLATFORM_XCB_KHR)
    find_path(NXRT_WAYLAND_CLIENT_INCLUDE wayland-client.h)
    if(NXRT_WAYLAND_CLIENT_INCLUDE)
        list(APPEND _nxrt_vk_platforms VK_USE_PLATFORM_WAYLAND_KHR)
    endif()
    target_compile_definitions(nxrt_volk PUBLIC VK_NO_PROTOTYPES ${_nxrt_vk_platforms})
else()
    target_compile_definitions(nxrt_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
endif()
set_target_properties(nxrt_volk PROPERTIES C_STANDARD 11)

# MoltenVK, the Vulkan driver on macOS. There is no Khronos release archive with a stable hash to pin here (the
# MoltenVK-macos.tar asset is rebuilt per tag), so the port takes it from the Vulkan SDK or from an explicit
# NXRT_MOLTENVK_DIR -- the layout the LunarG macOS SDK installs. volk dlopen()s "libvulkan.1.dylib" on Apple, so the
# library is copied next to each executable under that name (cmake/gfx.cmake) and found through @executable_path.
if(APPLE)
    set(NXRT_MOLTENVK_DIR "$ENV{VULKAN_SDK}/lib" CACHE PATH "Folder holding libMoltenVK.dylib (default: $VULKAN_SDK/lib)")
    find_library(NXRT_MOLTENVK_LIBRARY NAMES MoltenVK HINTS "${NXRT_MOLTENVK_DIR}" NO_DEFAULT_PATH)
    if(NOT NXRT_MOLTENVK_LIBRARY)
        find_library(NXRT_MOLTENVK_LIBRARY NAMES MoltenVK)
    endif()
    if(NOT NXRT_MOLTENVK_LIBRARY)
        message(FATAL_ERROR
            "libMoltenVK.dylib not found. Install the LunarG Vulkan SDK for macOS (https://vulkan.lunarg.com/sdk/home), "
            "or set NXRT_MOLTENVK_DIR to the folder that holds it.")
    endif()
    message(STATUS "MoltenVK: ${NXRT_MOLTENVK_LIBRARY}")
endif()

# VMA is header only; one translation unit in src/gfx defines VMA_IMPLEMENTATION.
add_library(nxrt_vma INTERFACE)
target_include_directories(nxrt_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")

# Dear ImGui core + its Vulkan backend (the platform side is fed from the nxrt input queue, so no SDL backend).
add_library(nxrt_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(nxrt_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(nxrt_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(nxrt_imgui PUBLIC nxrt_volk)
