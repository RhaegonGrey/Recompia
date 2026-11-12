# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-module lifting state (`Env`): image, function extents, imports (PLT), jump tables, landing pads."""
import bisect
import collections

from .lift import bits, sext
from . import eh as ehmod
from . import locals as localsmod
from . import spin as spinmod
from .sites import SITE_KINDS


class Env:
    """Per-module lifting state: the image, function extents, imports (PLT), jump tables, landing pads."""

    def __init__(self, img, cfg=None, full=True):
        from .config import GameConfig
        self.img = img
        self.cfg = cfg or GameConfig()
        self.base = img.base
        self.text_lo = img.base + img.text_start
        self.text_hi = self.text_lo + img.text_size
        self.full = full
        self.wanted = collections.OrderedDict()
        self.overrides = dict(self.cfg.overrides)
        self.imports = set()
        self.plt_cache, self.plt_slot = {}, {}
        self.plt_syms = None
        self.patches = {}
        self.sites = {k: set() for k in SITE_KINDS}
        self.module_index = 0         # NxModule.index: position in the program's module list (import ids)
        self.ends = {}
        self.starts = []
        self.jt = {}                  # address of a `br` -> sorted list of target addresses (jumptables.py)
        self.tailbr = set()           # addresses of unresolved `br xN` that are indirect tail calls (jumptables.py)
        self.pads = []                # sorted landing pad addresses
        self.catchers = set()         # entries of functions entered through nx_catch_call
        self.eh_starts = set()        # entries of functions that have an LSDA
        self.spin = False             # find spin loops and lift their loads as poll loads (spin.py; off while discovering)
        self.spin_yield = False       # ... and call nx_spin_hint(2) at their heads
        self.spin_stats = spinmod.Stats()
        self.locals = None            # None, 'exact', 'aapcs' or 'lean': registers and NZCV in C locals (locals.py; off while discovering)
        self.locals_stats = localsmod.Stats()
        self.flags_table = None       # 'lean': {entry: (NEEDS_IN, NEEDS_OUT, KILLS)} (locals.solve); None in the summary pass,
        self.locals_summaries = {}    # ... which fills this: {entry: locals.summarize()}
        self.br_plain = set()         # 'lean': unresolved `br`s whose target takes no table arithmetic (jumptables.bound_dispatch)
        self.lean_held = frozenset()  # 'lean': the code addresses the module's data and relocations hold (driver.held_code)
        self.lean_flows = {}          # 'lean': {entry: flag flow} the summary pass keeps for the rewrite (locals.apply)
        self.lean_cache = None        # ... in files in this folder, one per group of entries starting at lean_chunks
        self.lean_chunks = []
        self._syms = None

    # ---- words
    def word(self, addr):
        if self.patches and addr in self.patches:
            return self.patches[addr]
        return self.img.word(addr)

    # ---- imports through the PLT
    def load_plt(self):
        self.plt_syms, self.plt_info = {}, {}
        syms = self.img.symbols()
        for off, info, _ in self.img.jmprel():
            idx = info >> 32
            name, typ, bind, shndx, value, size = syms[idx]
            slot = self.base + off if off < self.base else off
            self.plt_syms[slot] = name
            self.plt_info[slot] = (idx, shndx, value)

    def plt_symbol(self, target):
        """If target is a PLT stub (adrp x16 / ldr x17 / add x16 / br x17), the imported symbol's name."""
        if target in self.plt_cache:
            return self.plt_cache[target]
        name = None
        if target + 16 <= self.text_hi and target >= self.text_lo and 23 in self.img.dt:
            w0, w1, w2, w3 = (self.word(target + i) for i in (0, 4, 8, 12))
            if (w0 & 0x9f00001f) == 0x90000010 and (w1 & 0xffc003ff) == 0xf9400211 and w3 == 0xd61f0220:
                if self.plt_syms is None:
                    self.load_plt()
                page = (target & ~0xfff) + (sext((bits(w0, 23, 5) << 2) | bits(w0, 30, 29), 21) << 12)
                name = self.plt_syms.get(page + bits(w1, 21, 10) * 8)
                if name is not None:
                    self.plt_slot[target] = page + bits(w1, 21, 10) * 8
        self.plt_cache[target] = name
        return name

    def plt_entry(self, target):
        """(symbol index, section index, value) when target is a PLT stub, else None."""
        if self.plt_symbol(target) is None:
            return None
        return self.plt_info[self.plt_slot[target]]

    # ---- direct calls
    def call(self, target):
        if target in self.overrides:
            return 'h_%s(c);' % self.overrides[target]
        if not (self.text_lo <= target < self.text_hi):
            return 'call_indirect(c, 0x%xULL);' % target          # another module / not code of this one
        self.wanted.setdefault(target, None)
        return 'f_%x(c);' % target

    # ---- function list
    def compute_starts(self, extra=()):
        """Function entries and exact ends: funcs.analyse(img) plus the config's extra entries (module offsets)."""
        from . import funcs
        rows, self.stats = funcs.analyse(self.img)
        self.ends = {}
        for r in rows:
            self.ends[self.base + r['start']] = self.base + r['end']
        for off in list(self.cfg.extra_entries.get(self.img.name, ())) + list(extra):
            a = self.base + off
            if a not in self.ends:
                self.ends[a] = self.base + off + 4
        self.starts = sorted(self.ends)
        # an extra entry splits the function it falls into: keep that function's end as is
        return self.starts

    def extent(self, entry):
        if entry in self.ends:
            return self.ends[entry]
        i = bisect.bisect_right(self.starts, entry)
        return self.starts[i] if i < len(self.starts) else self.text_hi

    # ---- exception data
    def load_eh(self):
        """Landing pads and the functions that need nx_catch_call, from the LSDAs."""
        rd = ehmod.Reader(self.img)
        fdes, cies = ehmod.all_fdes(self.img)
        pads, catchers, starts = set(), set(), set()
        self.eh_data = {}
        for f in fdes:
            if not f.lsda:
                continue
            sites, actions, types = ehmod.parse_lsda(rd, self.img, f)
            entry = self.base + f.start
            starts.add(entry)
            self.eh_data[entry] = (f, sites, actions, types)
            for s in sites:
                if not s.pad:
                    continue
                pad = self.base + s.pad
                pads.add(pad)
                a, has_catch = s.action, False
                while a:
                    filt, nxt = actions[a]
                    if filt > 0:
                        has_catch = True
                    a = nxt
                if has_catch and not self._terminate_pad(pad):
                    catchers.add(entry)
        self.pads = sorted(pads)
        self.catchers = catchers
        self.eh_starts = starts
        return fdes

    def _terminate_pad(self, pad):
        """A catch(...) pad that only calls __clang_call_terminate (a noexcept boundary): bl __cxa_begin_catch; bl terminate."""
        def bl_target(a):
            w = self.word(a)
            if (w & 0xfc000000) != 0x94000000:
                return None
            return a + sext(w & 0x3ffffff, 26) * 4
        t = None
        for i in range(2):
            t = bl_target(pad + 4 * i)
            if t:
                break
        if not t:
            return False
        names = []
        for i in range(6):
            x = bl_target(t + 4 * i)
            if x:
                names.append(self.symbol_name(x))
        return names[:2] == ['__cxa_begin_catch', '_ZSt9terminatev']

    def symbol_name(self, addr):
        n = self.plt_symbol(addr)
        if n:
            return n
        if self._syms is None:
            self._syms = {}
            for name, typ, bind, shndx, value, size in self.img.symbols():
                if shndx and name:
                    self._syms.setdefault(self.base + self.img.sym_value(value) if self.img.symtab_is_dynamic else value, name)
        return self._syms.get(addr)
