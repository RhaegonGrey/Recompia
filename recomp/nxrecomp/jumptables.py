# SPDX-License-Identifier: GPL-3.0-or-later
"""Jump-table resolution for `br xN`.

Adapted from SuperMarioWonderRecomp recomp/analysis.py (MIT License, Copyright (c) 2026 BelmanteGu; see
recomp/THIRD_PARTY.md): the backward search for the `add anchor, entry << shift` that computes the target, the table
read, and the bound from `cmp idx, #N; b.hi / b.hs`. Shapes understood:
  * clang's compressed tables:  adr/adrp+add anchor; ldrb/ldrh/ldrsb/ldrsh/ldrsw/ldr entry, [table, idx{, lsl #n}];
                                add target, anchor, entry{, lsl #s}; br target
  * table-relative ldrsw tables (anchor = the table itself), as GCC / older clang emit them
env.jt[br address] = sorted list of target addresses inside the function; the lifter then emits a switch over exactly
those targets (default: call_indirect, so a target outside the list still works). A `br` that does not match keeps the
fallback of smo-recomp (a dispatch over every instruction of the function), so nothing is ever lifted wrong.
"""
import re
import struct

import capstone

IMM = re.compile(r'#(-?0x[0-9a-f]+|-?\d+)')

NO_DEST = {'str', 'strb', 'strh', 'stur', 'sturb', 'sturh', 'stp', 'stlr', 'stlrb', 'stlrh', 'st1', 'st2', 'st3', 'st4',
           'cmp', 'cmn', 'tst', 'fcmp', 'fcmpe', 'ccmp', 'ccmn', 'fccmp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz',
           'tbz', 'tbnz', 'prfm', 'nop', 'udf', 'brk', 'svc', 'msr', 'dmb', 'dsb', 'isb', 'clrex', 'hint'}


def _imm(s):
    return int(s, 0)


def _regno(r):
    r = r.strip()
    if r in ('fp', 'x29', 'w29'):
        return 29
    if r in ('lr', 'x30', 'w30'):
        return 30
    m = re.fullmatch(r'[xw](\d+)', r)
    return int(m.group(1)) if m else r


def _ops(op_str):
    out, depth, cur = [], 0, ''
    for ch in op_str:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class _Fn:
    def __init__(self, start, end, insns):
        self.start, self.end, self.insns = start, end, insns


def _find_def(f, before, reg):
    a = before - 4
    while a >= f.start:
        mn, op = f.insns.get(a, ('', ''))
        if mn and mn not in NO_DEST:
            ops = _ops(op)
            if ops and _regno(ops[0]) == reg:
                return a, mn, ops
            if mn in ('ldp', 'ldpsw') and len(ops) > 1 and _regno(ops[1]) == reg:
                return a, mn, ops
        a -= 4
    return None


def _const(f, before, reg, depth=0):
    d = _find_def(f, before, reg)
    if not d or depth > 4:
        return None
    a, mn, ops = d
    if mn in ('adr', 'adrp'):
        return _imm(IMM.search(ops[1]).group(1))
    if mn == 'add' and len(ops) == 3 and ops[2].startswith('#') and _regno(ops[1]) != 'sp':
        base = _const(f, a, _regno(ops[1]), depth + 1)
        return None if base is None else base + _imm(ops[2][1:])
    if mn == 'mov' and ops[1].startswith('#'):
        return _imm(ops[1][1:])
    if mn == 'mov':
        return _const(f, a, _regno(ops[1]), depth + 1)
    return None


def _bound(f, before, idx):
    """Number of table entries from `cmp idx, #N` + b.hi / b.hs before the load (looks through uxtw / sxtw / mov / and copies)."""
    regs = {idx}
    a = before - 4
    while a >= max(f.start, before - 4 * 40):
        mn, op = f.insns.get(a, ('', ''))
        ops = _ops(op)
        copy = (len(ops) >= 2 and ops[1][:1] in ('x', 'w') and
                (mn in ('mov', 'uxtw', 'sxtw') or
                 (mn == 'and' and len(ops) == 3 and ops[2] in ('#0xffffffff', '#4294967295'))))
        if copy and _regno(ops[0]) in regs:
            regs.discard(_regno(ops[0]))
            regs.add(_regno(ops[1]))
            a -= 4
            continue
        if mn == 'cmp' and len(ops) == 2 and _regno(ops[0]) in regs and ops[1].startswith('#'):
            n = _imm(ops[1][1:])
            for k in range(1, 6):
                bm = f.insns.get(a + 4 * k, ('', ''))[0]
                if bm == 'b.hi':
                    return n + 1
                if bm == 'b.hs':
                    return n
            return None
        if mn and mn not in NO_DEST and ops and _regno(ops[0]) in regs:
            return None                                  # the index was produced here: an older cmp guards another value
        a -= 4
    return None


def _jump_table(img, f, br, shapes=None):
    reg = _regno(f.insns[br][1])
    d = _find_def(f, br, reg)
    if not d or d[1] != 'add' or len(d[2]) < 3:
        return None
    a_add, _, ops = d
    shift = 0
    m = re.search(r'(lsl|sxtw|uxtw) #(\d+)', ','.join(ops[2:]))
    if m:
        shift = int(m.group(2))
    ld = anchor = None
    for ent_op, anc_op in ((ops[2], ops[1]), (ops[1], ops[2])):
        cand = _find_def(f, a_add, _regno(ent_op))
        if cand and cand[1].startswith('ldr') and '[' in ','.join(cand[2]):
            ld = cand
            anchor = _const(f, a_add, _regno(anc_op))
            if anc_op is ops[2] and shift:
                return None
            break
    if ld is None or anchor is None:
        return None
    a_ld, ld_mn, ld_ops = ld
    size = {'ldrb': 1, 'ldrh': 2, 'ldrsb': 1, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 4}.get(ld_mn)
    signed = ld_mn in ('ldrsb', 'ldrsh', 'ldrsw')
    mem = re.match(r'\[(\w+)(?:, (\w+))?', ld_ops[1])
    if not size or not mem or not mem.group(2):
        return None
    base_reg, idx = _regno(mem.group(1)), _regno(mem.group(2))
    count = _bound(f, a_ld, idx)
    lo, hi = img.base, img.base + len(img.mem)

    def entries(table):
        limit = count if count is not None else 256
        out, misses = [], 0
        for k in range(limit):
            p = table + k * size
            if p + size > hi:
                break
            v = int.from_bytes(img.mem[p - img.base:p - img.base + size], 'little', signed=signed)
            t = anchor + (v << shift)
            if not (f.start <= t < f.end) or t not in f.insns:
                misses += 1
                if count is None and misses > 4:
                    break
                continue
            out.append(t)
        return out, misses

    table = _const(f, a_ld, base_reg)
    if table is not None and lo <= table < hi:
        out, _ = entries(table)
        if out and count is None and shapes is not None:     # read without a bound: lean_tables() may bound it
            shapes[br] = (table, size, signed, anchor, shift, f.start, f.end, frozenset(f.insns))
        return sorted(set(out)) if out else None
    if count is None:
        return None
    candidates, a = [], f.start
    while a < f.end:
        mn, op = f.insns.get(a, ('', ''))
        ops2 = _ops(op)
        if mn in ('adrp', 'adr', 'add') and ops2 and _regno(ops2[0]) == base_reg:
            c = _const(f, a + 4, base_reg)
            if c is not None and lo <= c < hi and c not in candidates:
                candidates.append(c)
        a += 4
    found = set()
    for c in candidates:
        out, misses = entries(c)
        if out and misses == 0:
            found.update(out)
    return sorted(found) if found else None


def _is_tail_br(img, f, br, depth=0):
    """True when the target of the unresolved `br` is a pointer taken from memory or passed in a register: an indirect
    tail call (virtual call through a vtable, function pointer, member-pointer thunk), never a computed jump inside the
    function. Compiled code computes in-function targets with `add anchor, entry << n` (jump tables), which is the
    pattern _jump_table resolves; a target produced by `add`/`adr` and not resolved stays a dispatch fallback.
    A load from a static table (adrp-based) counts only when no table entry points into this function."""
    reg = _regno(f.insns[br][1])
    d = _find_def(f, br, reg)
    if not d:
        return True                                        # parameter / caller-supplied register
    a, mn, ops = d
    if mn == 'mov' and len(ops) == 2 and ops[1][:1] in ('x', 'w') and depth < 4:
        src = _regno(ops[1])
        if isinstance(src, int):
            sub = _Fn(f.start, f.end, f.insns)
            d2 = _find_def(sub, a, src)
            return True if not d2 else _is_tail_br_def(img, sub, d2)
        return False
    return _is_tail_br_def(img, f, d)


def _is_tail_br_def(img, f, d):
    a, mn, ops = d
    if mn in ('ldp', 'ldpsw', 'ldur') and len(ops) > 2 and re.match(r'\[(sp|x29|fp)[,\]]', ops[-1] if mn != 'ldur' else ops[1]):
        return False                                       # a reload from the stack frame: may be a spilled jump-table target (-O0)
    if mn in ('ldp', 'ldpsw') or mn.startswith('ldur'):
        return True
    if mn.startswith('ldr') and len(ops) > 1 and re.match(r'\[(sp|x29|fp)[,\]]', ops[1]):
        return False                                       # same: `ldr x8, [sp, #n]` before `br x8` at -O0
    if mn.startswith('ldr') and len(ops) > 1 and '[' in ops[1]:
        m = re.match(r'\[(\w+), ([xw]\d+)(?:, lsl #(\d+))?\]', ops[1])
        if m:                                               # indexed load: a static table of pointers?
            base = _const(f, a, _regno(m.group(1)))
            if base is not None and img.base <= base < img.base + len(img.mem):
                for k in range(0, 256):
                    p = base + 8 * k
                    if p + 8 > img.base + len(img.mem):
                        break
                    v = img.ptr_at(p - img.base)
                    if f.start <= v < f.end:
                        return False                       # the table points into this function: a computed jump
        return True
    if mn == 'mov':
        return True
    return False


LOADS = re.compile(r'^ld(?:r|ur|p|xr|axr|ar|apr|np)')
STORES = re.compile(r'^st(?:r|ur|p|lr|xr|lxr|np)')
BRANCH_COND = re.compile(r'^(?:b\.\w+|cbn?z|tbn?z)$')


def _computed_br(img, f, brs):
    """{br: the address it jumps to may come from table arithmetic} for the `br`s of one function's code ('lean' only).
    A forward data flow over f's instructions: a register is tainted when it may hold an address computed from a
    constant by register arithmetic (`add/sub xD, xA, xE{, lsl|sxtw ...}` with a constant (adr / adrp / their
    add-immediate) or tainted operand: a jump table's anchor + entry, or anything else like it); every other instruction
    that writes a register taints it when one of its register operands is tainted. Memory is tainted once a tainted
    register is stored (a spilled target, -O0); loads give taint then. Calls leave their results untainted (code reached
    by a call computes its own table targets and jumps to them itself). Instructions entered from outside f start
    untainted. C: the registers that may hold a constant address (a may set: the join is the union)."""
    order = sorted(f.insns)
    succ = {}
    for a in order:
        mn, op = f.insns[a]
        ops = _ops(op)
        s = []
        t = None
        if mn == 'b' or BRANCH_COND.match(mn):
            m = IMM.search(ops[-1])
            t = _imm(m.group(1)) if m else None
            if t is not None and t in f.insns:
                s.append(t)
        if mn not in ('b', 'br', 'ret', 'braa', 'brab', 'retaa', 'retab'):
            if a + 4 in f.insns:
                s.append(a + 4)
        succ[a] = s
    NONE = (frozenset(), frozenset(), False)
    state = {a: None for a in order}
    work = [order[0]] if order else []
    state[order[0]] = NONE if order else None

    def join(a, st):
        old = state[a]
        new = st if old is None else (old[0] | st[0], old[1] | st[1], old[2] or st[2])
        if new != old:
            state[a] = new
            work.append(a)
    for a in order[1:]:                                  # code entered from elsewhere (a call into the middle, a pad)
        if state[a] is None and not any(a in succ[p] for p in (a - 4,) if p in succ):
            join(a, NONE)
    while work:
        a = work.pop()
        T, C, M = state[a]
        T, C = set(T), set(C)
        mn, op = f.insns[a]
        ops = _ops(op)
        regs = [_regno(o) for o in ops if re.fullmatch(r'[xw]\d+|fp|lr', o.strip())]
        dest = _regno(ops[0]) if ops and mn not in NO_DEST and re.fullmatch(r'[xw]\d+|fp|lr', ops[0]) else None
        if mn in ('bl', 'blr', 'blraa', 'blrab', 'blraaz', 'blrabz'):
            for r in list(range(19)) + [30]:
                T.discard(r)
                C.discard(r)
        elif STORES.match(mn):
            vals = [_regno(o) for o in ops[:2 if mn.startswith('stp') else 1]]
            if mn.startswith(('stxr', 'stlxr')):
                vals = [_regno(o) for o in ops[1:2]]
            if any(v in T for v in vals):
                M = True
        elif LOADS.match(mn):
            for r in ([_regno(ops[0]), _regno(ops[1])] if mn.startswith('ldp') else [dest]):
                C.discard(r)
                (T.add if M else T.discard)(r)
        elif dest is not None:
            src = regs[1:]
            if mn in ('adr', 'adrp'):
                C.add(dest)
                T.discard(dest)
            elif mn in ('add', 'sub') and len(ops) == 3 and ops[2].startswith('#'):
                c, t = bool(src) and src[0] in C, bool(src) and src[0] in T     # (sp: neither)
                (C.add if c else C.discard)(dest)
                (T.add if t else T.discard)(dest)
            elif mn == 'mov' and len(src) == 1:
                c, t = src[0] in C, src[0] in T
                (C.add if c else C.discard)(dest)
                (T.add if t else T.discard)(dest)
            elif mn in ('add', 'sub', 'adds', 'subs') and len(src) >= 2 and any(r in C or r in T for r in src):
                C.discard(dest)
                T.add(dest)
            else:
                C.discard(dest)
                (T.add if any(r in T for r in src) else T.discard)(dest)
                if mn in ('ldp', 'ldpsw') and len(regs) > 1:
                    (T.add if M else T.discard)(regs[1])
        st = (frozenset(T), frozenset(C), M)
        for b in succ[a]:
            join(b, st)
    out = {}
    for b in brs:
        st = state.get(b)
        out[b] = st is None or _regno(_ops(f.insns[b][1])[0]) in st[0]
    return out


def _data_refs(env):
    """The addresses the code names with `adrp xN` and an `add xM, xN, #lo12` or a load / store `[xN, #off]` within the
    next three instructions (none of them writing xN in between)."""
    words, ts, out = env.img.words(), env.text_lo, set()
    for i, w in enumerate(words):
        if (w & 0x9f000000) != 0x90000000:
            continue
        rd = w & 31
        imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
        page = ((ts + 4 * i) & ~0xfff) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
        for w2 in words[i + 1:i + 4]:
            if (w2 & 0xffc00000) == 0x91000000 and ((w2 >> 5) & 31) == rd:          # add xM, xN, #imm12
                out.add(page + ((w2 >> 10) & 0xfff))
                break
            if (w2 & 0x3b000000) == 0x39000000 and ((w2 >> 5) & 31) == rd:          # ldr / str [xN, #imm12 << size]
                out.add(page + (((w2 >> 10) & 0xfff) << (w2 >> 30)))
                break
            if (w2 & 31) == rd:                                                      # may write xN
                break
    return out


def lean_tables(env):
    """'lean' only: {br: targets} for the jump tables resolve() read without a bound (no `cmp idx, #N; b.hi`), cut to
    what the table can hold. Taken as true (named in the lift docs): the code never indexes a table past its end; a
    table ends before the next address the code names with adrp (_data_refs: another object starts there); every entry
    of a table lands on an instruction of the function of its br (the table is the run of such entries from its start).
    The lift's switch then lists only those targets; any other value still goes to call_indirect as before."""
    img = env.img
    refs = sorted(_data_refs(env))
    import bisect
    out = {}
    for br, (table, size, signed, anchor, shift, start, end, insns) in env.jt_shape.items():
        k = bisect.bisect_right(refs, table)
        stop = refs[k] if k < len(refs) else img.base + len(img.mem)
        tg = set()
        p = table
        while p + size <= stop:
            v = int.from_bytes(img.mem[p - img.base:p - img.base + size], 'little', signed=signed)
            t = anchor + (v << shift)
            if not (start <= t < end) or t not in insns:
                break
            tg.add(t)
            p += size
        tg = sorted(tg)
        if tg and tg != env.jt[br]:
            out[br] = tg
    return out


def bound_dispatch(env):
    """'lean' only (the lift is the same in every mode): env.br_plain = the unresolved, not tail `br`s (a dispatch over
    the whole function) whose target cannot come from table arithmetic (_computed_br): such a `br` reaches an address
    inside its function only if the program holds that address as a value (the lean summary's dispatch: an address in
    the module's data, a relocation, or a literal of the lifted code). The data flow runs over the code from the
    function entry before the `br` (FDE, symbol or `bl` target; not a hook site's) to the next one."""
    env.br_plain = set()
    img = env.img
    ts, words = env.text_lo, img.words()
    # function entries: the listed ones (FDEs, symbols; not a hook site's) and every `bl` target
    calls = {ts + 4 * i + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4 for i, w in enumerate(words)
             if (w & 0xfc000000) == 0x94000000}
    starts = sorted((set(env.starts) - env.sites['midhook']) | {a for a in calls if ts <= a < env.text_hi})
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    import bisect
    groups = {}
    for i, w in enumerate(words):
        if (w & 0xfffffc1f) == 0xd61f0000:
            b = ts + 4 * i
            if b in env.jt or b in env.tailbr or ((w >> 5) & 31) == 16:
                continue
            k = bisect.bisect_right(starts, b) - 1
            s = starts[k] if k >= 0 else ts
            if env.plt_symbol(s):
                continue
            e = starts[k + 1] if k + 1 < len(starts) else env.text_hi
            groups.setdefault((s, e), []).append(b)
    for (s, e), brs in groups.items():
        data = img.mem[s - img.base:e - img.base]
        insns = {}
        for a in range(s, e, 4):
            ins = list(cs.disasm(data[a - s:a - s + 4], a))
            if ins:
                insns[a] = (ins[0].mnemonic, ins[0].op_str)
        try:
            res = _computed_br(img, _Fn(s, e, insns), brs)
        except (KeyError, IndexError, ValueError, AttributeError, TypeError):
            continue
        env.br_plain |= {b for b, computed in res.items() if not computed}


def resolve(env):
    """Fill env.jt for every `br xN` (not x16 PLT stubs, not `ret`) of every listed function, and env.tailbr with the
    unresolved ones that are indirect tail calls."""
    env.jt = {}
    env.tailbr = set()
    env.jt_shape = {}
    img = env.img
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    words = img.words()
    ts = env.text_lo
    # functions that contain a `br xN`: br is 0xd61f0000 | Rn << 5
    br_at = [ts + 4 * i for i, w in enumerate(words) if (w & 0xfffffc1f) == 0xd61f0000]
    if not br_at:
        return
    import bisect
    starts = env.starts
    done = {}
    for b in br_at:
        k = bisect.bisect_right(starts, b) - 1
        if k < 0:
            continue
        s = starts[k]
        e = env.extent(s)
        if not (s <= b < e):
            continue
        if env.plt_symbol(s):
            continue
        f = done.get(s)
        if f is None:
            data = img.mem[s - img.base:e - img.base]
            insns = {}
            for a in range(s, e, 4):
                ins = list(cs.disasm(data[a - s:a - s + 4], a))
                if ins:
                    insns[a] = (ins[0].mnemonic, ins[0].op_str)
            f = done[s] = _Fn(s, e, insns)
        try:
            tg = _jump_table(img, f, b, env.jt_shape)
        except (KeyError, IndexError, ValueError, AttributeError, TypeError):
            tg = None
        if tg:
            env.jt[b] = tg
        else:
            try:
                if _is_tail_br(img, f, b):
                    env.tailbr.add(b)
            except (KeyError, IndexError, ValueError, AttributeError, TypeError):
                pass
