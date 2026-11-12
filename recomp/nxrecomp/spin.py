# SPDX-License-Identifier: GPL-3.0-or-later
"""Busy-wait (spin) loops: found per function, their loads lifted as loads the compiler must repeat.

A poll loop such as `L: ldr w8, [x0]; cbz w8, L` lifts to a C++ loop whose only exit depends on a plain load (rd32: a
memcpy). Nothing in that loop is a side effect the compiler has to keep: guest memory cannot alias the `Ctx *__restrict`,
so the load is loop invariant (LICM may hoist it, and spin forever), and C++'s forward-progress rule lets clang assume
the loop ends at all (at -O1 clang deletes `L: ldr; cbz L` outright and returns). Register locals (perf/m3-lifter) make
this certain: the c->x[] stores that still sit in the loop body today are gone there. The loads of a spin loop are
therefore lifted as relaxed atomic loads (ctx.h rdp8..rdp64 / rdvp, cxx.h nx::ld_poll): one real load per iteration,
the same instruction on x64, never hoisted, never merged, and a side effect for the forward-progress rule.

Criterion (pokopia-native's nxanalyze, after nxnative's rewriter): a backward B / B.cond / CBZ / CBNZ / TBZ / TBNZ at most
512 bytes back, inside the function, whose loop, from its head, runs only
  * loads (plain, acquire, exclusive; integer and FP/SIMD), register / flag / FP arithmetic, prefetches;
  * direct branches (B, B.cond, CBZ/CBNZ, TBZ/TBNZ), barriers (DMB, DSB, ISB), hints other than WFI / SEV / SEVL;
  * at most 4 stores whose address and value no pass of the loop changes (and no write-back);
and writes no register, vector register or flag that is live at the head: nothing is carried from one pass to the next,
so only another thread (a store to the polled memory) can end it. A counter or a cursor makes the loop end by itself and
is not a spin. Calls, SVCs, system registers (MRS/MSR: a timed wait reads the counter through the runtime, which the
compiler cannot see through anyway), returns, indirect branches and atomic read-modify-writes end the region. Several
back edges to one head form one loop if that whole region qualifies, else the qualifying back edges alone (one poll path
beside a counting one).

Kinds: poll, poll_store, wfe, yield (WFE / YIELD in the loop: they already call nx_spin_hint), register (no load: a
deliberate hang), and the acquire flag (an LDAR / LDAPR load, already atomic). With GameConfig.spin_yield
(`nxrecomp lift --spin-yield`, CMake NX_SPIN_YIELD, default off) the head of each poll loop also calls nx_spin_hint(2),
the runtime's spin hook (YieldProcessor, a host yield every 64th pass).

Branches, hints and barriers are decoded from their bit fields. The other instructions' register def/use comes from
Capstone's regs_access, with its alias error corrected (cmp / cmn / tst list the compared register as written). An error here can only move a loop between "spin" and
"not a spin": a non-spin loop with atomic loads is still correct, one load per pass as on the console.

# From pokopia-native (MIT), windows/src/nxanalyze/src/cfg.cpp @7c3655d: FnAnalyzer::spins and spin_region (the criterion,
# the per-back-edge retry, the store ban rounds, the liveness at the head, the kinds), ported to Python for this lifter.
"""
import re
import struct

LIMIT = 512                      # bytes from the head to a back edge
MAX_STORES = 4

POLL_SUB = (                     # the plain loads of the lifted statements -> their poll forms (ctx.h, cxx.h)
    (re.compile(r'\brd(8|16|32|64)\('), r'rdp\1('),
    (re.compile(r'\brdv\('), 'rdvp('),
    (re.compile(r'\bnx::ld<'), 'nx::ld_poll<'),
)
ATOMIC_LOAD = re.compile(r'__atomic_load_n|\bnx::ld_acq<|\bnx::ldar<|\bldxp128\(')

_cs = None


def _capstone():
    global _cs
    if _cs is None:
        import capstone
        _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        _cs.detail = True
    return _cs


# ---------------------------------------------------------------------------------------------------- one instruction
# classes
ALU, LOAD, LOAD_ACQ, LOAD_EXCL, STORE, PREFETCH, BRANCH, BARRIER, HINT, OTHER = range(10)

HINT_WFE, HINT_YIELD = 0xd503205f, 0xd503203f
HINT_BAD = (0xd503207f, 0xd503209f, 0xd50320bf)            # wfi, sev, sevl
RMW = re.compile(r'^(?:ld|st)(?:add|clr|eor|set|smax|smin|umax|umin)|^swp|^cas')
LOAD_EXCL_M = re.compile(r'^ld(?:a?x[rp])')
LOAD_ACQ_M = re.compile(r'^ld(?:ar|apr|apur|lar)')
STORE_EXCL_M = re.compile(r'^stl?x[rp]')
GPR = re.compile(r'^[xw](\d+)$')
VEC = re.compile(r'^[bhsdqv](\d+)$')
FLAG = 32                                                    # bit of NZCV in a register mask


def branch(w, pc):
    """(kind, target) of a direct branch: kind 'b' (unconditional) or 'bcond' (B.cond, CBZ/CBNZ, TBZ/TBNZ); else None."""
    if (w & 0xfc000000) == 0x14000000:
        return 'b', pc + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
    if (w & 0xff000010) == 0x54000000 or (w & 0x7e000000) in (0x34000000,):
        return 'bcond', pc + ((((w >> 5) & 0x7ffff) ^ 0x40000) - 0x40000) * 4
    if (w & 0x7e000000) == 0x36000000:
        return 'bcond', pc + ((((w >> 5) & 0x3fff) ^ 0x2000) - 0x2000) * 4
    return None


class Fx:
    __slots__ = ('cls', 'use', 'defs', 'vuse', 'vdef')

    def __init__(self, cls, use=0, defs=0, vuse=0, vdef=0):
        self.cls, self.use, self.defs, self.vuse, self.vdef = cls, use, defs, vuse, vdef


def _mask(cs, ins, regs):
    g = v = 0
    for r in regs:
        n = ins.reg_name(r)
        m = GPR.match(n)
        if m:
            g |= 1 << int(m.group(1))
        elif n in ('sp', 'wsp'):
            g |= 1 << 31
        elif n == 'fp':
            g |= 1 << 29
        elif n == 'lr':
            g |= 1 << 30
        elif n == 'nzcv':
            g |= 1 << FLAG
        else:
            m = VEC.match(n)
            if m:
                v |= 1 << int(m.group(1))
    return g, v


def effect(w, pc):
    """Fx of one instruction word: its class and the registers it reads and writes (bit n = x/w n, 31 = SP, 32 = NZCV;
    vector registers in vuse / vdef)."""
    op0 = (w >> 25) & 15
    if op0 in (10, 11):                                      # branches, exceptions, system
        br = branch(w, pc)
        if br:
            use = 0
            if (w & 0xff000010) == 0x54000000:
                use = 1 << FLAG
            elif (w & 0x7c000000) == 0x34000000:             # cbz/cbnz/tbz/tbnz read Rt
                use = 0 if (w & 31) == 31 else 1 << (w & 31)
            return Fx(BRANCH, use)
        if (w & 0xfffff01f) == 0xd503201f:
            return Fx(OTHER if w in HINT_BAD else HINT)
        if (w & 0xfffff0ff) in (0xd503309f, 0xd50330bf, 0xd50330df):
            return Fx(BARRIER)
        return Fx(OTHER)
    if op0 < 4:
        return Fx(OTHER)
    cs = _capstone()
    got = list(cs.disasm(struct.pack('<I', w), pc))
    if not got:
        return Fx(OTHER)
    ins = got[0]
    m = ins.mnemonic
    try:
        r, wr = ins.regs_access()
    except Exception:                                        # noqa: BLE001 (a Capstone decode gap: not part of a spin)
        return Fx(OTHER)
    use, vuse = _mask(cs, ins, r)
    defs, vdef = _mask(cs, ins, wr)
    if m in ('cmp', 'cmn', 'tst'):                           # aliases of subs/adds/ands wzr: Capstone lists Rn as written
        use |= defs & ~(1 << FLAG)
        vuse |= vdef
        defs, vdef = 1 << FLAG, 0
    if op0 in (4, 6, 12, 14):                                # loads and stores
        if RMW.match(m) or STORE_EXCL_M.match(m):
            return Fx(OTHER)
        if m.startswith('prf'):
            return Fx(PREFETCH)
        if m.startswith('ld'):
            cls = LOAD_EXCL if LOAD_EXCL_M.match(m) else LOAD_ACQ if LOAD_ACQ_M.match(m) else LOAD
            return Fx(cls, use, defs, vuse, vdef)
        if m.startswith('st'):
            return Fx(STORE, use, defs, vuse, vdef)
        return Fx(OTHER)
    return Fx(ALU, use, defs, vuse, vdef)


# ---------------------------------------------------------------------------------------------------- per function
class Loop:
    __slots__ = ('head', 'last', 'pcs', 'kind', 'acquire', 'loads')

    def __init__(self, head, last, pcs, kind, acquire, loads):
        self.head, self.last, self.pcs, self.kind, self.acquire, self.loads = head, last, pcs, kind, acquire, loads

    def __repr__(self):
        return 'Loop(%x..%x %s%s, %d insns)' % (self.head, self.last, self.kind, ' acquire' if self.acquire else '', len(self.pcs))


def find(word, entry, end):
    """Spin loops of the function [entry, end): [Loop], loads = the pcs of its plain loads (to lift as poll loads)."""
    n = (end - entry) // 4
    heads = {}
    for k in range(n):
        pc = entry + 4 * k
        br = branch(word(pc), pc)
        if br and entry <= br[1] <= pc and pc - br[1] <= LIMIT:
            heads.setdefault((br[1] - entry) // 4, []).append(k)
    if not heads:
        return []
    fx = {}

    def get(k):
        f = fx.get(k)
        if f is None:
            pc = entry + 4 * k
            f = fx[k] = effect(word(pc), pc)
        return f

    out = []
    for t in sorted(heads):
        backs = heads[t]
        lp = _region(word, entry, t, backs, get)
        if lp is None:
            valid = [b for b in backs if _region(word, entry, t, [b], get)]
            if not valid:
                continue
            lp = (len(valid) > 1 and _region(word, entry, t, valid, get)) or _region(word, entry, t, valid[:1], get)
        out.append(lp)
    return out


def _region(word, entry, t, backs, get):
    hi = max(backs)
    ban = set()
    for _round in range(8):
        def allowed(k):
            if k in ban:
                return False
            return get(k).cls not in (OTHER,)

        def nxt(k):
            pc = entry + 4 * k
            br = branch(word(pc), pc)
            s = []
            if br:
                tk = (br[1] - entry) // 4 if br[1] >= entry and (br[1] - entry) % 4 == 0 else -1
                if t <= tk <= hi:
                    s.append(tk)
                if br[0] == 'bcond' and k + 1 <= hi:
                    s.append(k + 1)
            elif k + 1 <= hi:
                s.append(k + 1)
            return s

        reach, todo = set(), [t]
        while todo:
            k = todo.pop()
            if k in reach or not allowed(k):
                continue
            reach.add(k)
            todo.extend(nxt(k))
        keep = {b for b in backs if b in reach}
        if not keep:
            return None
        more = True
        while more:                                          # instructions on a path from the head to a kept back edge
            more = False
            for k in range(hi, t - 1, -1):
                if k in reach and k not in keep and any(s != t and s in keep for s in nxt(k)):
                    keep.add(k)
                    more = True
        dg = dv = 0
        stores = loads = 0
        wfe = yld = cond = acquire = False
        for k in keep:
            f = get(k)
            dg |= f.defs
            dv |= f.vdef
            if f.cls == STORE:
                stores += 1
            elif f.cls in (LOAD, LOAD_ACQ, LOAD_EXCL):
                loads += 1
                acquire |= f.cls == LOAD_ACQ
            elif f.cls == HINT:
                w = word(entry + 4 * k)
                wfe |= w == HINT_WFE
                yld |= w == HINT_YIELD
            elif f.cls == BRANCH and branch(word(entry + 4 * k), entry + 4 * k)[0] == 'bcond':
                cond = True
        varying = False                                      # a store whose address or value a pass changes: not a spin path
        for k in keep:
            f = get(k)
            if f.cls == STORE and ((f.use & dg) or (f.vuse & dv) or f.defs):
                ban.add(k)
                varying = True
        if varying:
            continue
        live = {}                                            # live registers at each kept instruction, inside the loop only
        more = True
        while more:
            more = False
            for k in sorted(keep, reverse=True):
                og = ov = 0
                for s in nxt(k):
                    if s in keep:
                        g, v = live.get(s, (0, 0))
                        og |= g
                        ov |= v
                f = get(k)
                cur = (f.use | (og & ~f.defs), f.vuse | (ov & ~f.vdef))
                if live.get(k) != cur:
                    live[k] = cur
                    more = True
        lg, lv = live.get(t, (0, 0))
        if (lg & dg) or (lv & dv):
            return None                                      # a counter or a cursor: the loop ends by itself
        if (not loads and not wfe and not yld and not cond) or stores > MAX_STORES:
            return None                                      # `b .` alone is a deliberate hang, not a wait
        kind = 'wfe' if wfe else 'yield' if yld else 'register' if not loads else 'poll_store' if stores else 'poll'
        lds = sorted(entry + 4 * k for k in keep if get(k).cls in (LOAD, LOAD_EXCL))
        return Loop(entry + 4 * t, entry + 4 * hi, sorted(entry + 4 * k for k in keep), kind, acquire, lds)
    return None


# ---------------------------------------------------------------------------------------------------- lifted text
def poll_text(stmt):
    """The statement with its plain loads as poll loads, and whether it changed."""
    out = stmt
    for rx, rep in POLL_SUB:
        out = rx.sub(rep, out)
    return out, out != stmt


class Stats:
    def __init__(self):
        self.kinds = {}
        self.acquire = 0
        self.funcs = 0
        self.loads = 0            # load instructions lifted as poll loads
        self.unprotected = []     # (pc) plain loads in a spin loop whose lifted text has no load to rewrite (vector structure loads)
        self.yields = 0

    def add(self, o):
        for k, v in o.kinds.items():
            self.kinds[k] = self.kinds.get(k, 0) + v
        self.acquire += o.acquire
        self.funcs += o.funcs
        self.loads += o.loads
        self.unprotected += o.unprotected
        self.yields += o.yields

    @property
    def loops(self):
        return sum(self.kinds.values())

    def line(self, name):
        return ('%s: spin loops %d in %d functions (%s; %d with acquire loads), %d loads lifted as poll loads, %d head yields'
                % (name, self.loops, self.funcs, ', '.join('%s %d' % kv for kv in sorted(self.kinds.items())) or 'none',
                   self.acquire, self.loads, self.yields))


def apply(lines, loops, stats, yield_hint):
    """Rewrite the lifted statements [(pc, text)] of one function for its spin loops (find). Returns the new list."""
    if not loops:
        return lines
    stats.funcs += 1
    loads, heads = set(), set()
    for lp in loops:
        stats.kinds[lp.kind] = stats.kinds.get(lp.kind, 0) + 1
        stats.acquire += lp.acquire
        loads.update(lp.loads)
        if yield_hint and lp.kind not in ('wfe', 'yield'):
            heads.add(lp.head)
    last = lines[-1][0] if lines else None
    out = []
    for pc, stmt in lines:
        if pc in loads:
            stmt, changed = poll_text(stmt)
            if changed:
                stats.loads += 1
            elif not ATOMIC_LOAD.search(stmt):
                stats.unprotected.append(pc)
        if pc in heads and pc != last:
            stmt = 'nx_spin_hint(2); ' + stmt
            stats.yields += 1
        out.append((pc, stmt))
    return out
