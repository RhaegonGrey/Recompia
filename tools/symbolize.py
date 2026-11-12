#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Symbolizes what a pkp crash report, freeze dump or sampling profile carries.

The runtime prints host frames as absolute addresses (a crash report says `pc 0x10989114c` and `image base 0x102b00000`)
or as `pkp+0x<offset>` (a folded profile), and guest frames as guest addresses (0x7100…), because it carries no symbol
table. `atos` has the Mach-O symbol table and the lifted code's symbols are named `_f_<guest address>`, so both kinds
resolve here:

    tools/symbolize.py pkp_crash.txt                 # the report with names instead of addresses
    tools/symbolize.py pkp_freeze.txt
    tools/symbolize.py prof_samples.folded --top 20  # the 20 most frequent stacks

The image base is read from the report, or --base (default 0x100000000, the __TEXT vmaddr: offsets in a folded file are
relative to it, so a slide cancels out).
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

HOST_ADDR = re.compile(r"0x([0-9a-fA-F]{9,16})\b")
SKIP = ("region ", "image base ", "state ", "protect ", "writing address")
PKP_OFFSET = re.compile(r"\bpkp\+0x([0-9a-fA-F]+)")
BASE_LINE = re.compile(r"image base:\s*(0x[0-9a-fA-F]+)")
GUEST_LO, GUEST_HI = 0x7000000000, 0x8000000000


def nm_table(binary: str):
    """(host addresses sorted, guest VAs sorted, name by guest VA) for atos and the lifted-code names."""
    try:
        out = subprocess.run(["nm", "-n", binary], capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        sys.exit("nm not found (xcode-select --install)")
    hosts, guest_va, guest_name = [], [], {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            host = int(parts[0], 16)
        except ValueError:
            continue
        name = parts[2]
        hosts.append(host)
        if name.startswith("_f_") and len(name) > 3:
            try:
                va = int(name[3:], 16)
            except ValueError:
                continue
            guest_va.append(va)
            guest_name[va] = name[1:]
    order = sorted(range(len(guest_va)), key=guest_va.__getitem__)
    return hosts, [guest_va[i] for i in order], guest_name


def symbolicate(binary: str, base: int, offsets: list[int]) -> dict[int, str]:
    if not offsets:
        return {}
    out: dict[int, str] = {}
    addrs = [f"0x{base + off:x}" for off in offsets]
    for i in range(0, len(addrs), 512):
        chunk = addrs[i:i + 512]
        res = subprocess.run(["atos", "-o", binary, "-l", f"0x{base:x}", *chunk],
                             capture_output=True, text=True, check=False)
        for off, line in zip(offsets[i:i + 512], res.stdout.splitlines()):
            out[off] = line.strip()
    return out


def guest_lookup(guest_va: list[int], guest_name: dict[int, str], va: int) -> str | None:
    if not guest_va or not (GUEST_LO <= va < GUEST_HI):
        return None
    i = bisect.bisect_right(guest_va, va) - 1
    if i < 0:
        return None
    start = guest_va[i]
    return f"{guest_name[start]}+0x{va - start:x}" if va - start < 0x4000 else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--bin", default="work/build/game/pkp")
    ap.add_argument("--base", default=None, help="image base (default: the report's, else 0x100000000)")
    ap.add_argument("--top", type=int, default=0, help="for a .folded profile: print the N most frequent stacks")
    args = ap.parse_args()

    text = open(args.file, errors="replace").read()
    m = BASE_LINE.search(text)
    base = int(args.base, 0) if args.base else (int(m.group(1), 16) if m else 0x100000000)
    hosts, guest_va, guest_name = nm_table(args.bin)

    if args.file.endswith(".folded") and args.top:
        stacks = collections.Counter(l.split(";", 2)[-1] for l in text.splitlines() if l.startswith(";"))
        offsets = sorted({int(x, 16) for x in PKP_OFFSET.findall(text)})
        names = symbolicate(args.bin, base, offsets)
        for stack, n in stacks.most_common(args.top):
            line = PKP_OFFSET.sub(lambda mm: names.get(int(mm.group(1), 16), mm.group(0)), ";" + stack)
            print(f"{n:6d}  {line.lstrip(';')}")
        return 0

    # Host addresses: everything that looks like a loaded address (pc/lr/frame chain) or a pkp+offset.
    offset_names = symbolicate(args.bin, base, sorted({int(x, 16) for x in PKP_OFFSET.findall(text)}))
    host_addrs = sorted({int(x, 16) for x in HOST_ADDR.findall(text) if base <= int(x, 16) < GUEST_LO})
    host_names = symbolicate(args.bin, base, [a - base for a in host_addrs]) if host_addrs else {}
    host_by_addr = {base + off: name for off, name in host_names.items()}

    def fix(mm):
        tok = mm.group(0)
        if tok.lower().startswith("pkp+0x"):
            return offset_names.get(int(tok[6:], 16), tok)
        v = int(tok, 16)
        if GUEST_LO <= v < GUEST_HI:
            g = guest_lookup(guest_va, guest_name, v)
            return f"{tok} ({g})" if g else tok
        return host_by_addr.get(v, tok)

    def fix_line(l: str) -> str:
        if any(k in l for k in SKIP):
            return l
        return re.sub(r"pkp\+0x[0-9a-fA-F]+|0x[0-9a-fA-F]{9,16}\b", fix, l)

    print("\n".join(fix_line(l) for l in text.splitlines()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
