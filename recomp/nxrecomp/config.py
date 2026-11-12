# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-game configuration seam.

Everything game-specific that the generic lifter must not know lives in a `GameConfig` subclass under
recomp/games/<name>/config.py (and is selected with --game <name>). smo-recomp kept this knowledge inside its
lifter (sdklift_rules.txt, "nerve" RTTI naming, NPDM / version checks, base-to-update symbol matching); here the
lifter only asks the config. The default config is a neutral one that lifts a module as it is.
"""
import importlib
import importlib.util
import os


class ModuleSpec:
    """One module of the program: where it is mapped and how it is lifted. The game's builds (CMake, the player build) take
    their module table from here (`python -m nxrecomp build-info`); NxModule.index is the position in GameConfig.modules."""

    def __init__(self, name, base, source=None, kind='nso', shards=0, sites=None, entry=False):
        self.name = name              # 'main', 'sdk', 'subsdk0', ...
        self.base = base              # guest address of the module's first byte
        self.source = source          # file name of the module in the exefs, or None
        self.kind = kind              # 'nso' | 'nx_elf' | 'static_elf'
        self.shards = shards          # translation units of the generated code (lift --shards; 0 = by address)
        self.sites = sites            # site list (lift --sites), relative to recomp/games/<name>/, or None
        self.entry = entry            # the module the process starts in (its base is the entry point); loaded first


class GameConfig:
    name = 'generic'

    # modules of the program, in load order
    modules = []                      # [ModuleSpec]

    # guest address -> name of a host handler `h_<name>` that REPLACES the guest function (called as h_<name>(Ctx *c)).
    # Direct calls, indirect calls (call_indirect) and tail calls to that address all go to the handler.
    overrides = {}

    # extra function entries the discovery cannot see (offsets are module-relative), per module name
    extra_entries = {}

    # a function with more than this many instructions is compiled with optnone (bounded compile time); the build's
    # time-limit fallback (cmake/nx_cc_launcher.py) lowers it when a file still takes too long
    optnone_insns = 20000

    # guest registers and NZCV in C locals of each lifted function, synced with the Ctx only where other code can see it
    # (locals.py; `nxrecomp lift --locals off|exact|aapcs` overrides it for one run):
    #   'off'    the eager code: every instruction loads and stores the Ctx
    #   'exact'  at every call, return, tail exit and escape to the runtime the Ctx holds what the eager code stores there
    #   'aapcs'  the same for registers; NZCV is not stored at calls and returns nor loaded after a call (AAPCS64 passes
    #            no flags either way). Differs from 'exact' only where code reads flags across a call or a return, which
    #            the lifter counts per module ("may see the entry flags", "may see a callee flags")
    #   'lean'   the Ctx as 'exact' leaves it wherever code can look at it: NZCV is stored in front of a call / at a return
    #            / at a tail exit and loaded after a call only where a summary of the whole module says that the callee
    #            or a caller can read it; after a call only the registers that may be read again are loaded
    reg_locals = 'off'

    # spin loops (spin.py): their head also calls nx_spin_hint(2), the runtime's spin hook (`lift --spin-yield` turns it on too).
    # Their loads are lifted as poll loads either way.
    spin_yield = False


REG_LOCALS = ('off', 'exact', 'aapcs', 'lean')


def reg_locals_mode(cfg, override=None):
    """The locals mode of one lift: `override` (lift --locals) if given, else the game config's reg_locals; checked."""
    mode = override or cfg.reg_locals
    if mode not in REG_LOCALS:
        raise ValueError('reg_locals: %r is not one of %s' % (mode, ' / '.join(REG_LOCALS)))
    return mode


def load_game(name):
    """The GameConfig instance of recomp/games/<name>/config.py (class `Config`)."""
    if not name or name == 'generic':
        return GameConfig()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, 'games', name, 'config.py')
    spec = importlib.util.spec_from_file_location('nxrecomp_game_' + name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Config()
