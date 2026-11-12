#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reserve the guest address space in the Mach-O header of a macOS executable.

The runtime maps the guest's modules, heap, stacks and alias views at fixed addresses (runtime/include/nxrt/memory.h).
On macOS 15+ libmalloc's xzone allocator reserves two ~12 GiB ranges at random addresses inside 0x7000000000-0x8000000000
*before* main() runs (it mallocs during dyld/libSystem start-up), and the kernel keeps a 385 GiB guard region below
0x7000000000. A fixed guest layout inside that window therefore fails to reserve in most runs, and one inside the guard can
never be reserved at all.

The only mapping that exists before dyld is the executable image itself. This script appends a zero-fill segment named
__GUEST to the load commands, PROT_NONE, covering 0x7000000000-0x8200000000 (72 GiB). The kernel maps it at exec time, so
every later mmap(VM_FLAGS_ANYWHERE) - xzone's included - has to go around it. The runtime's own reservations inside that
range use MAP_FIXED (runtime/src/os/os_posix_mem.cpp).

Why the segment starts at 0x7000000000 and not lower: dyld slides the whole image up by a random amount (measured 0..84 MB
on macOS 27; the runtime refuses to start above 4 GiB), so the mapped range is [0x7000000000+slide, 0x8200000000+slide).
Starting at 0x7000000000 keeps it clear of the kernel's guard region below, and the lowest guest address (main at
0x7100000000) is 4 GiB above the start.

The link must leave room for the extra load command: pass -Wl,-headerpad,0x4000 (see nxrt_add_guest_segment in
runtime/cmake/gfx.cmake).

Usage: guest_segment.py <binary> [--vmaddr 0x...] [--size 0x...]
Exit status 0 when the segment is present (added now or already there), 1 with a message otherwise.
"""
import struct
import subprocess
import sys

LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xFEEDFACF
SEGMENT_NAME = b'__GUEST'
DEFAULT_VMADDR = 0x7000000000
DEFAULT_SIZE = 0x1200000000          # 72 GiB: 0x7000000000 - 0x8200000000


def die(msg):
    print('guest_segment: ' + msg, file=sys.stderr)
    sys.exit(1)


def main(argv):
    if len(argv) < 2:
        die(__doc__)
    path = argv[1]
    vmaddr, size = DEFAULT_VMADDR, DEFAULT_SIZE
    i = 2
    while i < len(argv):
        if argv[i] == '--vmaddr' and i + 1 < len(argv):
            vmaddr = int(argv[i + 1], 0)
            i += 2
        elif argv[i] == '--size' and i + 1 < len(argv):
            size = int(argv[i + 1], 0)
            i += 2
        else:
            die('unknown argument ' + argv[i])

    with open(path, 'rb') as f:
        data = bytearray(f.read())
    if len(data) < 32:
        die(path + ': too small to be a Mach-O')
    magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != MH_MAGIC_64:
        die(path + ': not a 64-bit Mach-O')

    # Walk the load commands once: find __GUEST (idempotence) and the end of the command area and of the first
    # file-backed content, which is where a new command has to fit.
    off = 32
    first_data = None
    have_guest = False
    for _ in range(ncmds):
        if off + 8 > len(data):
            die(path + ': truncated load commands')
        cmd, cmdsize = struct.unpack_from('<II', data, off)
        if cmdsize < 8 or off + cmdsize > len(data):
            die(path + ': bad load command size at %#x' % off)
        if cmd == LC_SEGMENT_64:
            name = bytes(data[off + 8:off + 24]).rstrip(b'\0')
            if name == SEGMENT_NAME:
                have_guest = True
            nsects = struct.unpack_from('<I', data, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                fileoff = struct.unpack_from('<I', data, so + 48)[0]
                if fileoff and (first_data is None or fileoff < first_data):
                    first_data = fileoff
                so += 80
        off += cmdsize
    end_cmds = 32 + sizeofcmds
    if have_guest:
        print('guest_segment: %s already has __GUEST' % path)
        return 0
    if first_data is None:
        first_data = len(data)
    if end_cmds + 72 > first_data:
        die('%s: no room for the load command (%d bytes free, 72 needed): link with -Wl,-headerpad,0x4000'
            % (path, first_data - end_cmds))

    # LC_SEGMENT_64, no sections, zero file size (a zero-fill segment, like __PAGEZERO), no access at all.
    command = struct.pack('<II', LC_SEGMENT_64, 72) + SEGMENT_NAME.ljust(16, b'\0') + struct.pack(
        '<QQQQiiII', vmaddr, size, 0, 0, 0, 0, 0, 0)
    data[end_cmds:end_cmds + 72] = command
    struct.pack_into('<II', data, 16, ncmds + 1, sizeofcmds + 72)
    with open(path, 'wb') as f:
        f.write(data)
    print('guest_segment: %s: __GUEST at %#x (+%#x)' % (path, vmaddr, size))

    # Editing the load commands invalidates the signature; an ad-hoc one is enough for a local build (M6 re-signs the
    # packaged .app after this step). A binary that was never signed is left alone if codesign is unavailable.
    try:
        subprocess.run(['codesign', '--force', '--sign', '-', path], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as e:
        die('%s: ad-hoc codesign failed (%s)' % (path, e))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
