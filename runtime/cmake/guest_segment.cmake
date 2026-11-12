# SPDX-License-Identifier: GPL-3.0-or-later
# Reserve the guest address space in a macOS executable (guest_segment.py).
#
# The runtime maps guest modules, the heap, stacks and alias views at fixed addresses (include/nxrt/memory.h). On macOS 15+
# libmalloc's xzone allocator reserves ~24 GiB at random addresses inside 0x7000000000-0x8000000000 before main() runs, and
# the kernel keeps a 385 GiB guard below it, so those reservations fail in most runs. The executable's own Mach-O header is
# the only mapping that exists earlier: a zero-fill __GUEST segment (0x7000000000, 72 GiB, PROT_NONE) makes the kernel map
# the range at exec time, and every later allocation goes around it. The runtime then uses MAP_FIXED inside it
# (src/os/os_posix_mem.cpp). See guest_segment.py for the slide allowance and the details.
set(_NXRT_GUEST_SEGMENT_PY "${CMAKE_CURRENT_LIST_DIR}/guest_segment.py" CACHE INTERNAL "guest_segment.py")

function(nxrt_add_guest_segment target)
    if(NOT APPLE)
        return()
    endif()
    # Room for the extra LC_SEGMENT_64 in the header (the linker otherwise packs the commands up to the first section).
    target_link_options(${target} PRIVATE "-Wl,-headerpad,0x4000")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        set(_py python3)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${_py} "${_NXRT_GUEST_SEGMENT_PY}" "$<TARGET_FILE:${target}>"
        COMMENT "Reserving the guest address space (__GUEST) in $<TARGET_FILE_NAME:${target}>"
        VERBATIM)
endfunction()
