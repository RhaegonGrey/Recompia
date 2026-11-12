# SPDX-License-Identifier: GPL-3.0-or-later
"""One guest function -> the text of its C++ function(s)."""
import bisect

from .lift import Lifter, Unimpl, bits
from . import locals as localsmod
from . import spin as spinmod


def dynimm_stmt(w, pc):
    """The instruction at pc with its immediate fetched from guest memory (add/sub immediate, movz, movn)."""
    sf, rd, rn = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5)
    T = 'uint64_t' if sf else 'uint32_t'
    if bits(w, 28, 23) == 0x22:                              # add / sub (immediate)
        fetch = 'uint32_t w_ = rd32(0x%xULL); uint64_t i_ = ((w_ >> 10) & 0xfff) << (((w_ >> 22) & 1) ? 12 : 0);' % pc
        a = 'c->x[%d]' % rn
        if bits(w, 29, 29):                                  # adds / subs: flags
            sub = bits(w, 30, 30)
            return ('{ %s %s r_ = adds%d(c, (%s)%s, %s, %d); %s }'
                    % (fetch, T, 64 if sf else 32, T, a, ('~(%s)i_' % T) if sub else '(%s)i_' % T, sub,
                       ('c->x[%d] = r_;' % rd) if rd != 31 else ''))
        op = '-' if bits(w, 30, 30) else '+'
        return '{ %s c->x[%d] = (%s)(%s %s i_); }' % (fetch, rd, T, a, op)
    if bits(w, 28, 23) == 0x25 and bits(w, 30, 29) in (0, 2):  # movn / movz
        neg = bits(w, 30, 29) == 0
        if rd == 31:
            return ';'
        return ('{ uint32_t w_ = rd32(0x%xULL); uint64_t i_ = (uint64_t)((w_ >> 5) & 0xffff) << (16 * ((w_ >> 21) & 3)); c->x[%d] = (%s)(%s); }'
                % (pc, rd, T, '~i_' if neg else 'i_'))
    raise Unimpl('dynimm: not an add/sub immediate, movz or movn')


TAILS = ('return;', 'goto', '{ jt', '{ f_', '{ h_', '{ call_indirect', 'switch (', '{ nx_')


def lift_function(env, entry):
    """-> (text, instruction count, [(pc, word, reason)], [(pad, entry)])."""
    img = env.img
    if env.full and env.plt_entry(entry):
        sym, shndx, value = env.plt_entry(entry)
        if shndx:                                    # symbol defined inside this module
            tgt = env.base + value
            env.wanted.setdefault(tgt, None)
            return 'void f_%x(Ctx *__restrict c) { f_%x(c); }' % (entry, tgt), 0, [], []
        env.imports.add(env.plt_symbol(entry))
        return 'void f_%x(Ctx *__restrict c) { host_import(c, NX_IMPORT_ID(%d, %d)); }' % (entry, env.module_index, sym), 0, [], []
    end = env.extent(entry)
    fn = Lifter(entry, end, env)
    lines, bad = [], []
    pc = entry
    while pc < end:
        w = env.word(pc)
        try:
            if pc in env.sites['dynimm']:
                stmt = dynimm_stmt(w, pc)
            else:
                stmt = fn.lift(w, pc)
        except Unimpl as e:
            stmt = 'guest_unimpl(0x%xULL, 0x%08xU, "%s");' % (pc, w, str(e).replace('"', "'"))
            bad.append((pc, w, str(e)))
        pre = ''
        if pc in env.sites['inline']:
            pre += 'nx_site_inline(c, 0x%xULL); ' % pc
        if pc in env.sites['midhook'] and pc != entry:
            pre += 'if (nx_site_midhook(c, 0x%xULL)) return; ' % pc
        if pre:
            stmt = '{ %s%s }' % (pre, stmt)
        lines.append((pc, stmt))
        pc += 4
    if env.spin and lines:
        lines = spinmod.apply(lines, spinmod.find(env.word, entry, end), env.spin_stats, env.spin_yield)
    pads = env.pads[bisect.bisect_left(env.pads, entry):bisect.bisect_left(env.pads, end)]
    two = bool(pads) or entry in env.catchers
    cold = ''
    if len(lines) > env.cfg.optnone_insns:
        cold = 'NX_OPTNONE '
    elif entry in env.sites['noinline'] or entry in env.sites['entryhook'] or entry in env.sites['midhook']:
        cold = '__attribute__((noinline)) '
    if not lines[-1][1].startswith(TAILS) and end < env.text_hi and end in env.ends:
        trailer = 'f_%x(c); return;' % end                 # falls through into the next listed function
        env.wanted.setdefault(end, None)
    elif not lines[-1][1].startswith(TAILS):
        trailer = 'guest_trap("fell off the end of a function", 0x%xULL); return;' % end
    else:
        trailer = 'return;'
    decl = None
    if env.locals:                                           # registers and NZCV in C locals (locals.py)
        skip = 'optnone' if cold.startswith('NX_OPTNONE') else 'unresolved br' if fn.has_br_fallback else None
        held = None
        if env.locals == localsmod.LEAN and env.flags_table is None and fn.has_br_fallback:
            brs = [pc for pc, t in lines if 'goto ' + localsmod.LABEL_DISPATCH in t]
            if brs and all(b in env.br_plain for b in brs):
                held = env.lean_held                         # the dispatch reaches only addresses the program holds
        res = localsmod.apply(lines, trailer, pads, env.locals, env.locals_stats, skip, entry, env.flags_table,
                              env.locals_summaries, held, env.lean_flows)
        if res:
            lines, trailer, decl = res
    out = ['%svoid %s_%x(Ctx *__restrict c%s) {' % (cold, 'u' if two else 'f', entry, ', uint64_t rp' if two else ''),
           '  c->fn = 0x%xULL;' % entry]
    if entry in env.sites['entryhook']:
        out.append('  if (nx_hook_entry(c, 0x%xULL)) return;' % entry)
    if decl:                                                 # after the entry hook: it may change registers and flags
        out.append('  ' + decl)
    if fn.has_br_fallback:
        out.append('  uint64_t jt = 0;')
    if pads:
        out.append('  if (rp) switch (rp) {')
        out.extend('  case 0x%xULL: goto L_%x;' % (lp, lp) for lp in pads)
        out.append('  default: guest_trap("resume at an address that is not a landing pad of this function", rp); return; }')
    needed = set(fn.targets) | set(pads)
    if fn.has_br_fallback:
        needed = {pc for pc, _ in lines}
    for pc, stmt in lines:
        out.append(('L_%x: %s' % (pc, stmt)) if pc in needed else '  ' + stmt)
    out.append('  ' + trailer)
    if fn.has_br_fallback:
        out.append('L_dispatch: switch (jt) {')
        out.extend('  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines)
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    if two:
        out.append('void f_%x(Ctx *__restrict c) { %s }' % (entry, 'nx_catch_call(c, u_%x);' % entry if entry in env.catchers else 'u_%x(c, 0);' % entry))
    return '\n'.join(out), len(lines), bad, [(lp, entry) for lp in pads]
