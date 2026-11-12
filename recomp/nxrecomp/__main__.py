# SPDX-License-Identifier: GPL-3.0-or-later
"""python -m nxrecomp <command> ...   (see README.md)

  lift   <input> <outdir> [--game NAME] [--kind nso|nx_elf|static_elf] [--base 0x..] [--name N] [--jobs J] [--shards S]
         [--patches FILE] [--sites FILE]... [--spin-yield] [--locals off|exact|aapcs|lean]   lift every function of one module
  cover  <input> [--game NAME] ...   per-instruction-word translation coverage of .text
  build-info [--game NAME]   one JSON line: the game's module table and the compile flags of the lifted code (for CMake)
  module-header --game NAME --out FILE   the C++ header of the module list (modheader.py), for the builds of the lifted game
  funcs  -> python -m nxrecomp.funcs
"""
import argparse
import json
import sys


def spec_from(a):
    kind = a.kind
    if not kind:
        magic = open(a.input, 'rb').read(4)
        kind = 'nso' if magic == b'NSO0' else 'static_elf'
    return {'kind': kind, 'path': a.input, 'base': int(a.base, 16), 'name': a.name, 'game': a.game, 'patches': a.patches, 'sites': a.sites, 'index': a.index,
            'spin_yield': getattr(a, 'spin_yield', False) or None, 'locals': getattr(a, 'locals', None)}


def build_info(game, arch=''):
    """The module table of the game config and the compile flags (compile_flags.py) for the target architecture, as one JSON-ready dict."""
    from . import compile_flags as cf
    from .config import load_game
    mods = [{'name': m.name, 'file': m.source or m.name, 'base': '0x%x' % m.base, 'index': i, 'shards': m.shards,
             'sites': m.sites or ''} for i, m in enumerate(load_game(game).modules)]
    return {'modules': mods, 'clang_cl_opt': cf.CLANG_CL_OPT,
            'clang_cl': cf.for_arch(arch, 'clang_cl'), 'clang': cf.for_arch(arch, 'clang')}


def main(argv=None):
    from .config import REG_LOCALS
    ap = argparse.ArgumentParser(prog='nxrecomp')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('build-info')
    p.add_argument('--game', default='generic')
    p.add_argument('--arch', default='', help='target processor (CMAKE_SYSTEM_PROCESSOR): -mcx16 is added for x86-64 only')
    p = sub.add_parser('module-header')
    p.add_argument('--game', required=True)
    p.add_argument('--out', required=True)
    for name in ('lift', 'cover'):
        p = sub.add_parser(name)
        p.add_argument('input')
        if name == 'lift':
            p.add_argument('outdir')
            p.add_argument('--jobs', type=int, default=8)
            p.add_argument('--shards', type=int, default=0)
            p.add_argument('--spin-yield', action='store_true', help='spin-loop heads call nx_spin_hint(2) (default: the game config)')
            p.add_argument('--locals', choices=REG_LOCALS,
                           help='guest registers and flags in C locals (locals.py; modes: config.py reg_locals); default: the game config')
        p.add_argument('--game', default='generic')
        p.add_argument('--kind', choices=['nso', 'nx_elf', 'static_elf'])
        p.add_argument('--base', default='0x7100000000')
        p.add_argument('--name', default='main')
        p.add_argument('--patches')
        p.add_argument('--index', type=int, help='module index (NxModule.index, import ids); default: position in the game config')
        p.add_argument('--sites', action='append',
                       help='site list: patches, inline callouts, hooks (see sites.load_sites); repeat to merge several by address')
    a = ap.parse_args(argv)
    if a.cmd == 'build-info':
        print(json.dumps(build_info(a.game, a.arch)))
        return 0
    if a.cmd == 'module-header':
        from . import modheader
        from .config import load_game
        modheader.write(load_game(a.game).modules, a.out)
        return 0
    spec = spec_from(a)
    from . import cover, driver
    if a.cmd == 'lift':
        driver.lift_module(spec, a.outdir, a.jobs, a.shards)
        return 0
    from .config import load_game
    img = driver.open_image(spec)
    ok, bad, _ = cover.cover(img, load_game(a.game))
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())
