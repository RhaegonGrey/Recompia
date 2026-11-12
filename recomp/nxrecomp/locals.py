# SPDX-License-Identifier: GPL-3.0-or-later
"""Guest registers and NZCV in C locals: a rewrite of one lifted function's statements (emit_function.lift_function).

The instruction lifters write every guest register and flag through the context (c->x[n], c->N ..., adds64(c, ...)), so
each guest instruction is a load and a store of Ctx memory, four stores per flag-setting instruction. The compiler removes
such a store only where it proves that nothing reads the Ctx before the next one, and it keeps no register in a host
register across a branch join. This pass moves them into locals that clang keeps in host registers and syncs with the Ctx
only where something else can see it. Modes (GameConfig.reg_locals, `nxrecomp lift --locals`): 'off' (this pass is not
run: the eager code), 'exact', 'aapcs' and 'lean' (below).

Registers. Every X register the function names (x0..x30, SP = x[31]) becomes a local `xN`. Before a statement part that
hands the Ctx to other code (a call of a lifted function, call_indirect, an SVC, MRS/MSR to the runtime, a site callout or
hook, a trap: escapes()) the registers that may differ from the Ctx are stored (spill), and a `return` spills first. Which
ones may differ is a forward data flow over the gotos and fall-throughs (dirty sets, may), followed in text order inside a
statement. After a lifted call the Ctx is the truth again: the callee, the runtime it reaches or an unwinder may have
changed any register. Locals are loaded lazily: a statement that reads a register whose local is not valid on every path
to it (valid sets, must) loads it from the Ctx first, which is current there because the register is dirty on no path. A
register dirty on one path and not valid on another (written in one branch only) is "eager": loaded at the entry and right
after every call, so it is always valid. After any other escape every local is loaded again (clang drops the loads nobody
reads). So wherever other code runs, the Ctx holds the registers the eager code would have stored there.

Flags. NZCV lives in a local `NxF F` (include/nxrecomp/locals.h): the packed N Z C V and the operands of the last
flag-setting add, subtract or logic instruction. Every definition writes both; a reader whose reaching definitions are
all of one kind (sub64, add32, logic64, ...) computes its condition from the operands, so `cmp x1, x2; b.lt` becomes
one signed compare; any other reader uses the packed form, and so does a reader that follows a reload of F inside its own
statement (a site hook in front of the instruction may have changed the Ctx's flags). clang drops what nobody reads, which
makes the flags lazy. F starts from the Ctx at the entry and is stored into the Ctx at tail exits (a branch to another
function, the fall into the next listed function, an indirect tail call) and at escapes to the runtime, which then reload
it; only where a definition can reach (F differs from the Ctx's flags otherwise). 'exact' also stores F before every
lifted call and at every return and loads it after the call, so the Ctx's NZCV is what the eager code leaves at every call
and return. 'aapcs' does neither: AAPCS64 neither passes NZCV to a callee nor returns it, so compiled code never reads
flags across a call or a return. A read that can see flags from before its function's entry or from a callee is counted
(Stats entry_reads, after_call; the lift log lists them with their addresses): such reads are where 'aapcs' differs from
'exact'. 'lean' gives the result of 'exact' with the syncs only where the whole module can look at them: it stores F in
front of a call only if the callee may read its entry flags or the caller reads the flags after it (the callee may leave
them as they were), loads it after a call only if the caller looks at the flags after it, and stores it at a return
only if a caller looks at them (the flag table: the 'lean' section below). It also loads after a call only the eager
registers that are read before being written again.

What still sees a stale Ctx. Between two sync points the Ctx's x[] and NZCV are behind the locals. Code that runs there
without being called by the lifted function reads old values: another thread that looks at this thread's Ctx (the
runtime's freeze and crash reports print its lr / sp / fp and registers) and a host fault handler on one of the function's
own memory accesses. Both must treat the Ctx of a running thread as a hint. Everything the function calls, and every way
out of it (return, tail exit, an exception or longjmp out of a callee), finds the Ctx exact. The landing pads of a
function are entered through a new call of it (u_<entry>(c, pad)), so they start from the Ctx like the entry does.

Left as they were (apply() returns None and Stats.left counts the reason): functions with an unresolved `br` (their
dispatch can enter any instruction), functions compiled with optnone (no SROA, the locals would only add stack traffic),
any function whose text uses c->x with a computed index or takes a register's address, any with a register write under
control flow other than a mid-function hook's `if (...) return;`, any with an escape that is not a tail exit under
control flow (its sync would run on one path only), and any that reads a register after a lifted call inside one statement
(only the eager registers are loaded there). The lifter emits none of the last two today; they guard the pass against a
new statement form.

Design after valentderah/totk-pc-recomp recomp/regs.py and recomp/flags.py (MIT License, Copyright (c) 2026 Valiantsin
Dzerakh; see recomp/THIRD_PARTY.md): dirty-set spills, conditions from the operands of the reaching definition kind,
AAPCS64 flags. Re-implemented for this lifter's statements; differences: every register in locals instead of the 4-6 most
used, loaded lazily; no trust in callee-saved registers after a call; every flag definition writes both forms instead of
a per-definition packing plan.
"""
import re

REG = re.compile(r'c->x\[(\d+)\]')
WRITE = re.compile(r'c->x\[(\d+)\]\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
# a register used other than by value: its address taken (a unary &: one that follows no operand; `a & c->x[1]` is an AND),
# a computed index, ++ / --
ODD_REG = re.compile(r'(?:^|[(,=;{}?:!~+\-*/%<>|^&])\s*&\s*c->x\b|c->x\[(?!\d+\])|c->x\[\d+\]\s*(?:\+\+|--)|(?:\+\+|--)\s*c->x\[')
GOTO = re.compile(r'goto L_([0-9a-f]+);')
LABEL_DISPATCH = 'L_dispatch'

# flag helpers that take the Ctx (ctx.h, cxx.h); rewritten to take F
FLAG_CALL = re.compile(r'\b(?:nx::)?(adds64|adds32|subs64|subs32|logic64|logic32|set_nzcv|get_nzcv|cond|fcmp)\b(\s*<[^<>()]*>)?\(c\b')
FLAG_RAW = re.compile(r'c->([NZCV])\b')
FLAG_DEF_NAMES = ('adds64', 'adds32', 'subs64', 'subs32', 'logic64', 'logic32', 'set_nzcv', 'fcmp')

# helpers taking the Ctx that touch neither X registers nor NZCV (exclusive monitor, vector registers): no sync needed
SAFE = frozenset(['ldxr', 'stxr', 'clrex', 'ldxp', 'stxp', 'ldxp128', 'stxp32', 'stxp64', 'stx8', 'stx16', 'stx32', 'stx64',
                  'v_tbl', 'v_ldn', 'v_stn', 'stx_diag'])
# a call that passes the Ctx: NAME(c, ... ) or NAME(c)
CALL_C = re.compile(r'\b((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*(<[^<>()]*>)?\(c\s*[,)]')
# calls that do not pass the Ctx but end in the runtime's crash report
NO_CTX_ESCAPE = re.compile(r'\b(guest_trap|guest_unimpl)\s*\(')
# statements after which control never falls through to the next one (exact forms only: a wrong match would be unsafe)
TERMINAL = re.compile(r'^(?:return;|goto L_[0-9a-f]+;|\{ (?:[fh]_\w+\(c\)|u_[0-9a-f]+\(c, 0\)|call_indirect\(c, [^;{}]*\)); return; \})$')
# calls that leave for good when followed by `return;` (tail exits: publish the flags, no reload)
TAILCALL_NAMES = re.compile(r'^(?:[fhu]_\w+|call_indirect)$')

# the same calls inside a tail exit (`<call>; return;`): what is left of a statement without them has only escapes that come back
TAIL_EXIT = re.compile(r'(?:[fhu]_\w+\(c(?:, 0)?\)|call_indirect\(c, [^;{}]*\));\s*return;')
LEAN = 'lean'
MODES = ('exact', 'aapcs', LEAN)

# a call of a lifted function of this module whose address the lifter knows (f_<addr>, or u_<addr> of a two-entry one)
DIRECT = re.compile(r'^[fu]_([0-9a-f]+)$')
# runtime calls that run no lifted code on this thread and leave the Ctx's NZCV as they found it (lean: the flags after
# them are the ones published in front of them). The site hooks run guest code only through the game layer's call helper,
# which restores the whole Ctx; no runtime or game-layer code writes c->N/Z/C/V (see the 'lean' section below).
FLAT_RUNTIME = frozenset(['guest_svc', 'guest_mrs', 'guest_msr', 'guest_trap', 'guest_unimpl', 'nx_site_midhook',
                          'nx_site_inline', 'nx_hook_entry'])

KINDS = ('add64', 'sub64', 'add32', 'sub32', 'logic64', 'logic32')
P = 'p'           # packed flags only (CCMP, FCMP, MSR NZCV, ADCS / SBCS)
CTX = 'ctx'       # F equals the Ctx's flags (a reload after the runtime ran); read packed
ENTRY = 'entry'   # F equals the flags the function was entered with (from the Ctx); read packed


def _close(text, i):
    """Index just past the parenthesis that closes the one at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                return j + 1
    raise ValueError('unbalanced parentheses: ' + text)


def _args(text, i):
    """Top-level argument strings of the call whose '(' is at text[i], and the index past its ')'."""
    end = _close(text, i)
    out, depth, start = [], 0, i + 1
    for j in range(i + 1, end - 1):
        ch = text[j]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == ',' and depth == 0:
            out.append(text[start:j].strip())
            start = j + 1
    out.append(text[start:end - 1].strip())
    return out, end


def flag_effect(text):
    """(reads flags, kind written or None, written on every path) of one statement's original text."""
    reads = bool(FLAG_RAW.search(text))
    defs = []
    for m in FLAG_CALL.finditer(text):
        name = m.group(1)
        if name in ('cond', 'get_nzcv'):
            reads = True
            continue
        if name in ('adds64', 'adds32'):
            args, _ = _args(text, m.end() - 2)
            cin = args[3] if len(args) == 4 else ''
            if cin == '0':
                defs.append('add' + name[4:])
            elif cin == '1':
                defs.append('sub' + name[4:])
            else:                                  # ADCS / SBCS: carry in from the flags
                reads = True
                defs.append(P)
        elif name in ('subs64', 'subs32'):
            defs.append('sub' + name[4:])
        elif name in ('logic64', 'logic32'):
            defs.append('logic' + name[5:])
        else:                                      # set_nzcv, fcmp
            defs.append(P)
    if not defs:
        return reads, None, False
    kind = defs[0] if len(defs) == 1 else P
    always = len(defs) == 1 and 'if (' not in text and '?' not in text
    return reads, kind, always


def calls_out(text):
    """Where the statement calls code that can see the Ctx: the text offsets of every call that takes it and is not a
    helper known to touch neither registers nor flags, and of the traps."""
    out = [m.start() for m in NO_CTX_ESCAPE.finditer(text)]
    for m in CALL_C.finditer(text):
        name = m.group(1).split('::')[-1]
        if not (name in SAFE or name in FLAG_DEF_NAMES or name in ('cond', 'get_nzcv')):
            out.append(m.start())
    return out


def escapes(text):
    """Whether the statement hands the Ctx to other code (and so needs a sync)."""
    return bool(calls_out(text)) or 'return;' in text


def conditional_escape(text):
    """Whether an escape that comes back (not a tail exit) may sit under control flow (some comes before it in the
    statement): its sync would run on one path only, which the per-statement data flow does not model. A mid-function
    hook's call is the condition itself: it always runs."""
    rest = TAIL_EXIT.sub('', MIDHOOK.sub('nx_site_midhook(c, 0);', text))
    return any(CONTROL.search(rest[:i]) or '?' in rest[:i] for i in calls_out(rest))


class Odd(Exception):
    """A statement form the rewrite does not handle: the function stays as it is (the text is the reason)."""


def _cfg(stmts, pads):
    """(texts, index, succ, falls, roots) of a function's statements: gotos and fall-throughs (falls[i]: statement i can
    fall into i + 1). An unresolved br (`goto L_dispatch`) may go to any statement."""
    texts = [t for _, t in stmts]
    n = len(texts)
    index = {pc: i for i, (pc, _) in enumerate(stmts) if pc is not None}
    succ, falls = [], []
    for i, t in enumerate(texts):
        s = [index[int(m.group(1), 16)] for m in GOTO.finditer(t) if int(m.group(1), 16) in index]
        if 'goto ' + LABEL_DISPATCH in t:
            s = list(range(n))
        f = i + 1 < n and not TERMINAL.match(t.strip())
        if f:
            s.append(i + 1)
        succ.append(s)
        falls.append(f)
    roots = [0] + [index[p] for p in pads if p in index]
    return texts, index, succ, falls, roots


def plan(stmts, pads=(), flags='exact', entry=None, table=None, flow=None):
    """-> (texts, decl, Stats) with the locals rewrite applied; raises Odd when the function must stay as it is.
    stmts: [(pc or None, text)] in order; pads: landing pad addresses (entered from the resume switch).
    'lean': entry is the function's address, table the program's flag table (solve()), flow: what summarize() kept of
    flag_flow() for these statements (None: follow it here)."""
    texts = [t for _, t in stmts]
    for t in texts:
        if LABEL_DISPATCH in t:
            raise Odd('unresolved br')
        if ODD_REG.search(t):
            raise Odd('register by address or computed index')
    texts, index, succ, falls, roots = _cfg(stmts, pads)
    n = len(texts)

    regs = sorted({int(m.group(1)) for t in texts for m in REG.finditer(t)})
    writes = [{int(m.group(1)) for m in WRITE.finditer(t)} for t in texts]
    # registers a statement reads: every mention that is not the target of a plain `=` (a compound one reads too)
    reads = [{int(m.group(1)) for m in REG.finditer(t)} - plain_targets(t) for t in texts]
    esc = [escapes(t) for t in texts]
    fx = [flag_effect(t) for t in texts]
    for t, w in zip(texts, writes):
        # writes are taken as done on every path through their statement; the only control flow the lifter puts in
        # front of a register write is a mid-function hook's `if (...) return;`
        if w and CONTROL.search(MIDHOOK.sub('', t)):
            raise Odd('register write under control flow')
    for t, e in zip(texts, esc):
        if e and conditional_escape(t):
            raise Odd('escape under control flow')
    lean = [None] * n
    if flags == LEAN:                              # what each call, return and tail exit does with F (flag_flow, solve)
        obs, sites, isites = flow or flag_flow(texts, succ, roots, falls, index)[:3]
        nin, kills = table.nin, table.kills
        out_self = table.nout(entry)
        lean = [LeanStmt(out_self, table) for _ in range(n)]
        for i, j, g in sites:
            # take F back from the Ctx after the call when something looks at what the call leaves before a definition;
            # store it in front when the callee may read it, or may leave it as it found it (not KILLS) and F is taken
            # back after
            ra = holds(obs.get(('c', i, j), ()), nin, out_self, table.ind_in, kills)
            lean[i].calls.append((nin(g) or (ra and not kills(g)), ra))
        for i, j in isites:
            ra = holds(obs.get(('i', i, j), ()), nin, out_self, table.ind_in, kills)
            lean[i].icalls.append((table.ind_in or ra, ra))
    # what each statement's syncs leave (rewrite's `after`): None, or ('all' | 'eager', registers written after them);
    # reload_f: its rewrite reloads F from the Ctx (an escape other than a tail exit; in 'aapcs' also not a lifted call),
    # def_last: ... and a flag definition follows the last such reload in its text
    probe = [rewrite(t, regs, (), (), P, True, True, flags, lean[i]) if e else ('', None) for i, (t, e) in enumerate(zip(texts, esc))]
    reload_f = [RELOAD_F in p for p, _ in probe]
    def_last = [bool(FX_DEF.search(p[p.rfind(RELOAD_F):])) for p, _ in probe]
    info = [a for _, a in probe]

    # dirty registers on entry to each statement (may: union over predecessors). Not reached in the flow: none.
    dirty = [None] * n
    work = []
    for r in roots:
        if dirty[r] is None:
            dirty[r] = set()
            work.append(r)
    while work:
        i = work.pop()
        out = (dirty[i] | writes[i]) if info[i] is None else set(info[i][1])
        for s in succ[i]:
            if dirty[s] is None or not out <= dirty[s]:
                dirty[s] = (dirty[s] or set()) | out
                work.append(s)
    dirty = [d if d is not None else set() for d in dirty]

    # Registers in locals are loaded lazily: before a statement that reads one whose local is not valid on every path
    # (must: intersection over predecessors), the local is loaded from the Ctx, which is then current because the
    # register is not dirty on any path. A register that is dirty on one path and not valid on another (written in
    # one branch, untouched in the other, then read or spilled) is "eager": loaded at the entry and after every call,
    # so its local is always valid. Iterate until no such conflict is left.
    preds = [[] for _ in range(n)]
    for i, s in enumerate(succ):
        for j in s:
            preds[j].append(i)
    allr = set(regs)
    eager = set()
    rel = lambda j: eager                          # the registers loaded again after the lifted call(s) of statement j
    if flags == LEAN:
        # only those an instruction may read before writing them, or a spill may store, after the call: the others'
        # locals are never looked at before they are written again (liveness, below). A statement with two lifted
        # calls reloads them all (the first call's reload would have to cover the second's spill).
        live = live_after(succ, reads, writes, [dirty[i] if esc[i] else set() for i in range(n)])
        many = [sum(1 for m in CALL_C.finditer(t) if TAILCALL_NAMES.match(m.group(1).split('::')[-1])) > 1 for t in texts]
        rel = lambda j: eager if many[j] else (eager & live[j])
    while True:
        valid = [set(allr) for _ in range(n)]
        for r in roots:
            valid[r] = set(eager)
        loads = [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n):
                if i in roots:
                    vin = set(eager)
                elif preds[i]:
                    vin = set.intersection(*[vout(j, valid, loads, writes, info, rel(j), allr) for j in preds[i]])
                else:
                    vin = set(eager)                   # not reached in the flow
                if vin != valid[i]:
                    valid[i] = vin
                    changed = True
                ld = reads[i] - vin
                if ld != loads[i]:
                    loads[i] = ld
                    changed = True
        conflict = set()
        for i in range(n):
            conflict |= dirty[i] - valid[i]
        if not conflict - eager:
            break
        eager |= conflict

    # kinds of the flag definitions reaching each statement ('call': a call since then, for stats only)
    reach = [None] * n
    work = []
    for r in roots:
        if reach[r] is None:
            reach[r] = {ENTRY}
            work.append(r)
    while work:
        i = work.pop()
        reads, kind, always = fx[i]
        out = set(reach[i])
        if reload_f[i]:
            out = {CTX}                            # the runtime ran: F reloaded from the Ctx
        if kind and (def_last[i] or not reload_f[i]):  # (a definition before the reload went to the Ctx and came back)
            out = {kind} if always else (out | {kind})
        elif info[i] is not None and info[i][0] == 'eager':
            out = out | {'call'}
        for s in succ[i]:
            if reach[s] is None or not out <= reach[s]:
                reach[s] = (reach[s] or set()) | out
                work.append(s)
    for i in range(n):
        if reach[i] is None:
            reach[i] = {P}                         # not reached in the flow

    st = Stats()
    out_texts = []
    for i, t in enumerate(texts):
        reads, kind, _ = fx[i]
        mode = None
        if reads:
            ks = reach[i] - {'call'}
            mode = next(iter(ks)) if len(ks) == 1 and ks <= set(KINDS) else P
            if reload_f[i]:
                mode = P                           # F was reloaded inside this statement: the operands are not its flags
            st.reads += 1
            st.operand += mode != P
            st.after_call += 'call' in reach[i]
            st.entry_reads += ENTRY in reach[i]
            if ENTRY in reach[i] or 'call' in reach[i]:
                st.where.append((stmts[i][0], 'entry' if ENTRY in reach[i] else 'call'))
        # F differs from the Ctx's flags only after a definition: publish at an exit only then
        publish = bool(kind) or bool(reach[i] - {CTX, ENTRY, 'call'})
        pre = ''.join('x%d = c->x[%d]; ' % (r, r) for r in sorted(loads[i]))
        out_texts.append(pre + rewrite(t, regs, dirty[i], sorted(rel(i)), mode, esc[i], publish, flags, lean[i], st)[0])
    decl = ''
    if eager:
        decl += 'uint64_t ' + ', '.join('x%d = c->x[%d]' % (r, r) for r in sorted(eager)) + '; '
    lazy = [r for r in regs if r not in eager]
    if lazy:
        decl += 'uint64_t ' + ', '.join('x%d' % r for r in lazy) + '; '
    decl += 'NxF F = {c->N, c->Z, c->C, c->V, 0, 0};'
    st.eager = len(eager)
    st.lazy = len(lazy)
    st.funcs = 1
    return out_texts, decl, st


class Stats:
    def __init__(self):
        self.funcs = self.reads = self.operand = self.after_call = self.entry_reads = 0
        self.where = []               # (pc, 'entry' | 'call') of those reads
        self.eager = self.lazy = 0     # registers loaded at the entry / on first use
        self.left = {}                # functions left as they were: reason -> count
        # lifted calls that come back / of them with F stored in front, F loaded after; returns / with F stored;
        # registers loaded again after those calls (sum)
        self.calls = self.call_pub = self.call_reload = self.rets = self.ret_pub = self.reloads = 0

    def leave(self, reason):
        self.left[reason] = self.left.get(reason, 0) + 1

    def add(self, o):
        self.funcs += o.funcs
        self.reads += o.reads
        self.operand += o.operand
        self.after_call += o.after_call
        self.entry_reads += o.entry_reads
        self.where += o.where
        self.eager += o.eager
        self.lazy += o.lazy
        self.calls += o.calls
        self.call_pub += o.call_pub
        self.call_reload += o.call_reload
        self.rets += o.rets
        self.ret_pub += o.ret_pub
        self.reloads += o.reloads
        for k, v in o.left.items():
            self.left[k] = self.left.get(k, 0) + v

    def lines(self, name, mode):
        """The lift log's lines for one module."""
        out = ['%s: locals (%s) in %d functions; flag reads %d: from operands %d, may see the entry flags %d, may see a callee flags %d'
               % (name, mode, self.funcs, self.reads, self.operand, self.entry_reads, self.after_call),
               '  registers in locals: %d loaded at the entry, %d on first use' % (self.eager, self.lazy),
               '  lifted calls %d: flags stored in front %d, loaded after %d, registers loaded after %.2f on average; returns %d: flags stored %d'
               % (self.calls, self.call_pub, self.call_reload, self.reloads / float(self.calls or 1), self.rets, self.ret_pub),
               '  left as they were: %d functions (%s)' % (sum(self.left.values()),
                                                           ', '.join('%s %d' % kv for kv in sorted(self.left.items())) or 'none')]
        if self.where:
            out.append('  those reads: ' + ' '.join('%x(%s)' % (pc or 0, k) for pc, k in sorted(self.where, key=lambda w: w[0] or 0)[:40]))
        return out


def apply(lines, trailer, pads, mode, stats, skip=None, entry=None, table=None, summaries=None, held=None, flows=None):
    """The statements [(pc, text)] of one function and its closing statement on the locals: (lines, trailer, declarations),
    or None when the function stays as it is (`skip`: the caller's reason, else plan's). stats: Stats, updated.
    'lean' needs the program's flag table (solve()); without one this is the summary pass: the function's summary goes
    into `summaries` (entry -> summarize()) and its text stays as it is. held: for a function whose unresolved brs
    take no table arithmetic (jumptables.bound_dispatch), the code addresses the module holds in its data (the driver's
    held_code()): the summary is then the one with the dispatch limited by held_targets(), and summaries[HELD, entry]
    = (the other statements' addresses, the summary without the limit) lets the driver check the limit against every
    function's literals (check_held()). flows: {entry: the flag flow of the summary pass} ('lean'), filled by the
    summary pass and read by the rewrite (the same statements give the same flow: it is not followed twice)."""
    if mode == LEAN and table is None:
        stmts = lines + [(None, trailer)]
        if held is None:
            summaries[entry] = summarize(stmts, pads, keep=flows, entry=entry)
        else:
            reach = held_targets(stmts, held)
            summaries[entry] = summarize(stmts, pads, reach)
            others = frozenset(pc for pc, _ in lines) - reach
            summaries[HELD, entry] = (others, summarize(stmts, pads) if others else None)
        return None
    if skip:
        stats.leave(skip)
        return None
    try:
        texts, decl, st = plan(lines + [(None, trailer)], pads, mode, entry, table, (flows or {}).get(entry))
    except Odd as e:
        stats.leave(str(e))
        return None
    stats.add(st)
    return [(pc, t) for (pc, _), t in zip(lines, texts)], texts[-1], decl


PUBLISH = 'c->N = F.N; c->Z = F.Z; c->C = F.C; c->V = F.V; '
RELOAD_F = ' F.N = c->N; F.Z = c->Z; F.C = c->C; F.V = c->V;'


FX_DEF = re.compile(r'\bfx_(?:%s)\b' % '|'.join(FLAG_DEF_NAMES))
LOCAL_READ = re.compile(r'(?<![\w.>\[])x\d+\b(?!\s*=(?!=))')
LOCAL_WRITE = re.compile(r'(?<![\w.>\[])x(\d+)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
PLAIN_TARGET = re.compile(r'c->x\[(\d+)\]\s*=(?!=)')
CONTROL = re.compile(r'\bif \(|\belse\b|\bswitch \(|\bfor \(|\bwhile \(')
MIDHOOK = re.compile(r'if \(nx_site_midhook\(c, 0x[0-9a-f]+ULL\)\) return;')


def plain_targets(text):
    """Registers whose every mention in text is the target of a plain `=` (written, not read)."""
    targets = {int(m.group(1)) for m in PLAIN_TARGET.finditer(text)}
    counts = {}
    for m in REG.finditer(text):
        counts[int(m.group(1))] = counts.get(int(m.group(1)), 0) + 1
    plain = {}
    for m in PLAIN_TARGET.finditer(text):
        plain[int(m.group(1))] = plain.get(int(m.group(1)), 0) + 1
    return {r for r in targets if plain[r] == counts[r]}


def vout(j, valid, loads, writes, info, eager, allr):
    """Registers whose local is valid after statement j."""
    after = info[j]
    if after is None:
        return valid[j] | loads[j] | writes[j]
    kind, wr = after
    return (allr if kind == 'all' else eager) | wr


def rewrite(text, regs, dirty, eager, mode, escape, publish=True, flags='exact', lean=None, count=None):
    """-> (text, after): the statement on the locals (registers and flags renamed, syncs around every escaping call and
    return) and what its last non-tail sync leaves: None (no sync), or ('all' | 'eager', registers written after it).
    dirty: registers that may differ from the Ctx on entry; eager: the registers reloaded after a lifted call (every
    register is reloaded after other escapes). lean: LeanStmt ('lean' mode); count: Stats to count the syncs in."""
    # 1. flags: helper calls take F; reads use the chosen mode
    def flag_sub(m):
        name, targs = m.group(1), m.group(2) or ''
        if name == 'cond':
            return ('fx_cc_%s(F' % mode) if mode != P else 'fx_cond(F'
        return 'fx_%s%s(F' % (name, targs)
    text = FLAG_CALL.sub(flag_sub, text)
    text = FLAG_RAW.sub(lambda m: 'F.' + m.group(1), text)
    # 2. registers
    text = REG.sub(lambda m: 'x' + m.group(1), text)
    if not escape:
        return text, None
    pub = PUBLISH if publish else ''
    rl_all = ''.join(' x%d = c->x[%d];' % (r, r) for r in regs)
    rl_eager = ''.join(' x%d = c->x[%d];' % (r, r) for r in eager)
    # 3. syncs, in text order: every escaping call and every return; register writes in between grow the dirty set
    sites = []
    for m in CALL_C.finditer(text):
        name = m.group(1).split('::')[-1]
        if name in SAFE or name.startswith('fx_'):
            continue
        sites.append((m.start(), 1, name))
    for m in NO_CTX_ESCAPE.finditer(text):
        sites.append((m.start(), 1, m.group(1)))
    for m in re.finditer(r'\breturn;', text):
        sites.append((m.start(), 1, 'return'))
    for m in LOCAL_WRITE.finditer(text):
        # the write happens after its right-hand side: `x0 = (({ ...; auto r_ = guest_mrs(c, ...); reload; r_; }))`
        # stores x0 after the escape's sync, so it is dirty after it (placed at the assignment's start, the reload
        # dropped it from the dirty set and the next call reloaded the stale Ctx value)
        sites.append((_assign_end(text, m.end()), 0, int(m.group(1))))
    sites.sort()
    d = set(dirty)
    after = None
    out, pos = [], 0
    k = ki = 0                                     # lean: index of the next direct call / call_indirect
    for start, is_site, name in sites:
        if not is_site:
            d.add(name)
            if after is not None:
                after[1].add(name)
            continue
        if start < pos:                            # the return of a tail exit, consumed with its call
            continue
        sp = ''.join('c->x[%d] = x%d; ' % (r, r) for r in sorted(d))
        out.append(text[pos:start])
        if name == 'return':
            p = pub if (lean.ret if lean else flags != 'aapcs') else ''
            out.append('{ %s%sreturn; }' % (sp, p))
            pos = start + len('return;')
            if count is not None:
                count.rets += 1
                count.ret_pub += bool(p)
            continue
        end = _close(text, text.index('(', start))
        call = text[start:end]
        tail = re.match(r'\s*;\s*return;', text[end:])
        stmt = re.match(r'\s*;', text[end:]) and _stmt_start(text, start)
        lifted = bool(TAILCALL_NAMES.match(name))
        direct = DIRECT.match(name)
        if tail and lifted:                        # tail exit: spill, publish the flags, leave
            h = int(direct.group(1), 16) if direct else None
            p = pub if not (lean and direct) or lean.table.nin(h) or (lean.ret and not lean.table.kills(h)) else ''
            out.append('{ %s%s%s; return; }' % (sp, p, call))
            pos = end + tail.end()
            continue
        if stmt and lifted:                        # a call of a lifted function: only it can change registers
            if lean:                               # flag_flow: per known callee; through call_indirect as 'exact'
                pb, rf = lean.calls[k] if direct else lean.icalls[ki]
                k, ki = k + bool(direct), ki + (not direct)
            else:
                pb = rf = flags != 'aapcs'         # AAPCS64: a callee neither reads nor leaves flags
            out.append('{ %s%s%s;%s%s }' % (sp, pub if pb else '', call, rl_eager, RELOAD_F if rf else ''))
            if count is not None:
                count.calls += 1
                count.call_pub += bool(pub and pb)
                count.call_reload += rf
                count.reloads += len(eager)
            after = ('eager', set())
        elif stmt:                                 # the runtime: it may read or change anything
            out.append('{ %s%s%s;%s%s }' % (sp, pub, call, rl_all, RELOAD_F))
            after = ('all', set())
        else:                                      # the call's value is used: a statement expression (clang)
            out.append('({ %s%sauto r_ = %s;%s%s r_; })' % (sp, pub, call, rl_all, RELOAD_F))
            after = ('all', set())
        pos = end + (re.match(r'\s*;', text[end:]).end() if stmt else 0)
        d = set()
        if after[0] == 'eager' and LOCAL_READ.search(text[pos:]):
            raise Odd('register read after a call in one statement')   # only the eager registers were loaded again
    out.append(text[pos:])
    return ''.join(out), after



def _assign_end(text, i):
    """Index of the end of the assignment whose right-hand side starts at text[i]: the first ; or , at its own
    bracket depth, or the bracket that closes around it."""
    depth = 0
    for j in range(i, len(text)):
        ch = text[j]
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            if depth == 0:
                return j
            depth -= 1
        elif ch in ';,' and depth == 0:
            return j
    return len(text)


def _stmt_start(text, i):
    """Whether the call at text[i] starts a statement (its value is not used): after ; { } else, or after the condition
    of an if (not after a cast)."""
    j = i - 1
    while j >= 0 and text[j] == ' ':
        j -= 1
    if j < 0 or text[j] in ';{}' or text[max(0, j - 3):j + 1] == 'else':
        return True
    if text[j] != ')':
        return False
    depth = 0
    for k in range(j, -1, -1):
        if text[k] == ')':
            depth += 1
        elif text[k] == '(':
            depth -= 1
            if depth == 0:
                return text[max(0, k - 3):k].rstrip().endswith('if')
    return False


# ---- 'lean': flags across calls, from a summary of the whole module -------------------------------------------------
#
# 'exact' stores F into the Ctx in front of every lifted call and at every return and loads it after every call, because
# the callee may read the flags it is entered with and the caller may read the flags the callee leaves. Compiled code
# almost never does either, but the lifter makes it happen (a function split at a branch target or a hook site is
# entered with live flags). flag_flow() follows, inside one function, where the flags present at its entry and the flags
# each call leaves can be looked at before a definition replaces them ("observed"), and by what:
#   'R'    a flag read
#   g      a call of / tail exit to the lifted function g of this module: observed if g reads its entry flags (NEEDS_IN g)
#   'X'    a call or tail exit into code reached from outside the direct calls (call_indirect, an import, a runtime call
#          that runs guest code): observed if such code may read its entry flags (FlagTable.ind_in, below)
#   'out'  a return or a tail exit (the target may leave them as they are): observed if a caller reads this function's
#          flags after calling it (NEEDS_OUT self)
# A runtime call that runs no guest code and leaves NZCV alone (FLAT_RUNTIME) observes nothing and changes nothing: the
# rewrite stores F in front of it where a definition reaches and loads it back after, so F keeps its value and sources.
# summarize() keeps that per function; solve() runs over every function of the module (the driver's summary pass, before
# any function is rewritten) to the least fixpoint of
#   NEEDS_IN(f)   the entry flags of f are observed in f
#   NEEDS_OUT(g)  f calls g and the flags g leaves are observed in f; f tail-exits (or falls) to g and NEEDS_OUT(f);
#                 g's caller is outside the lifted code (the module's entry point: the host reads the flags it leaves)
# Passing through a callee. The flags in front of a direct call of g are still there after it unless g defines them, so
# flag_flow() tags each value with the set of direct callees it went through (its "via") and an observation of it holds
# only if no callee in its via KILLS. KILLS(g) is the greatest fixpoint of: every way from g's entry (or a landing pad)
# to a return or a tail exit defines the flags, or goes through a call of / tail exit to a function that KILLS (via of
# the E/P 'out' observations, summarize()). Taken as not KILLS: a function of another module, an import, anything(). A
# call of a KILLS function is a definition (what it leaves is its own); one that does not KILLS passes the caller's
# flags on. Vias are kept small (_norm): a smaller via is a weaker condition, so dropping callees from one only adds
# observations.
# plan() then stores F in front of a call of g only if NEEDS_IN(g), or if what the call leaves is observed and g does not
# KILLS (the flags may come back through g); loads it after the call only if what the call leaves is observed; stores it at a return
# only if NEEDS_OUT(self) and at a tail exit to g only if NEEDS_IN(g) or NEEDS_OUT(self) and g does not KILLS. An indirect
# tail exit and the runtime calls keep the 'exact' syncs. A mid-function hook's early return leaves the function, so a
# definition after it in its statement is on every path that goes on (_flag_events). A function the rewrite leaves as
# it is (unresolved br, optnone) gets its summary from its own text all the same (the eager code keeps the flags in the
# Ctx: the same flow); one whose text flag_flow cannot follow (an escape under control flow) gets anything(): it reads
# its entry flags and what everything it calls leaves.
#
# Where an unresolved br goes inside its function. Any statement, unless no table arithmetic can reach its target
# register (jumptables.bound_dispatch: a data flow over the function's instructions); then only the statements whose
# address the program holds as a value: in its data or relocations (driver.held_code), as a literal of the lifted code
# (adr, adrp + add, a return address) or in a 4 KiB page an adrp literal names (held_targets, check_held). Taken as
# true: code addresses come only from those and from table arithmetic (the same as for "entered", below), and a target
# computed by table arithmetic is jumped to by a br of the function that computed it. A jump table read without a
# bound is cut to the run of entries before the next address the code names with adrp (jumptables.lean_tables; the
# assumptions are named there); the lift's switch lists only those targets.
#
# Code reached from outside the direct calls: the module's entry point, its exported symbols, the game config's extra
# entries, and every function whose address the module's code or data may hold ("entered", from the driver). Whoever
# enters one that way (call_indirect, an import's caller in another module, the runtime calling back from inside an
# import) may read the flags it leaves, and no module's lift sees another's table, so every entered function has
# NEEDS_OUT, and the exported ones and the entry point are external as well. An import is a runtime call: the flags are
# stored in front of it and loaded after it as in 'exact'. Their entry flags: solve() first takes ind_in = False and
# checks it: no entered function may have NEEDS_IN; if one does, ind_in = True (F is stored in front of every
# call_indirect) and the fixpoint is taken again (the lift log names them).
#
# The rewrite needs the same flow the summary pass followed (the same statements): summarize() keeps the part plan()
# uses, and the driver keeps it on disk between the passes.

class FlagTable(dict):
    """{entry: (NEEDS_IN, NEEDS_OUT, KILLS)} of a module ('lean'), and ind_in: code entered from outside the direct calls
    may read its entry flags; ind_out: every function entered that way stores its flags at returns (always, solve()).
    A function not in the table (another module's) is taken as reading them, and as not KILLS."""
    ind_in = ind_out = True

    def nin(self, g):
        return self.get(g, (True, True, False))[0]

    def nout(self, g):
        return self.get(g, (True, True, False))[1]

    def kills(self, g):
        return self.get(g, (True, True, False))[2]


class LeanStmt:
    """The 'lean' syncs of one statement: calls / icalls [(store F in front, load F after)] per call of a lifted
    function of this module / per call_indirect, in text order; ret: store F at a return; table: the FlagTable (a tail
    exit to g stores F if NEEDS_IN(g), or if ret and g may leave the flags it was entered with)."""
    def __init__(self, ret, table):
        self.calls, self.icalls, self.ret, self.table = [], [], ret, table


def holds(conds, nin, out, ind_in, kills=lambda g: False):
    """Whether one of the observations in conds happens (see above): conds holds (what, via) pairs; via: the direct
    callees the flags passed through on the way, any of which KILLS means they cannot get there."""
    for c, via in conds:
        if via and any(kills(g) for g in via):
            continue
        if c == 'R' or (c == 'out' and out) or (c == 'X' and ind_in) or (c not in ('out', 'X') and nin(c)):
            return True
    return False


VIA_MAX = 8     # callees remembered per way through calls (fewer conditions: the flags are taken to get through more)


def _norm(items):
    """The (source, via) pairs of a state, reduced: per source only the smallest vias (a larger one gets through only
    where a smaller one does), at most VIA_MAX of them (else their intersection: it gets through wherever one does).
    The result does not depend on the order of items."""
    by = {}
    for b, v in items:
        by.setdefault(b, set()).add(v)
    out = []
    for b, vs in by.items():
        if len(vs) > 1:
            if frozenset() in vs:
                vs = [frozenset()]
            else:
                keep = []
                for v in sorted(vs, key=len):
                    if not any(u <= v for u in keep):
                        keep.append(v)
                vs = [frozenset.intersection(*keep)] if len(keep) > VIA_MAX else keep
        out.extend((b, v) for v in vs)
    return frozenset(out)


def _flag_events(t):
    """[(pos, order, kind, arg)] of one statement's original text in text order: flag reads and definitions (arg: the
    definition is on every path), calls ('call' g, 'icall' call_indirect, 'ind' a runtime call that may run guest code,
    'flat' one that does not), tail exits ('tail' g or None), returns, gotos and the jump of an unresolved br."""
    ev = []
    for m in FLAG_RAW.finditer(t):
        ev.append((m.start(), 0, 'read', None))
    # A mid-function hook's early return leaves the function (its 'ret' event comes first): the rest of the statement runs
    # on the one path that goes on, so a definition there is on every path that reaches what follows.
    always = flag_effect(MIDHOOK.sub('nx_site_midhook(c, 0);', t))[2]
    for m in FLAG_CALL.finditer(t):
        name = m.group(1)
        if name in ('cond', 'get_nzcv'):
            ev.append((m.start(), 0, 'read', None))
            continue
        if name in ('adds64', 'adds32'):
            args, _ = _args(t, m.end() - 2)
            if (args[3] if len(args) == 4 else '') not in ('0', '1'):
                ev.append((m.start(), 0, 'read', None))      # ADCS / SBCS: carry in
        ev.append((_close(t, m.end() - 2), 1, 'def', always))
    consumed = set()
    for pos in calls_out(t):
        m = NO_CTX_ESCAPE.match(t, pos)
        name = m.group(1) if m else CALL_C.match(t, pos).group(1).split('::')[-1]
        end = _close(t, t.index('(', pos))
        tail = re.match(r'\s*;\s*return;', t[end:])
        d = DIRECT.match(name)
        if tail and TAILCALL_NAMES.match(name):
            consumed.add(end + tail.end() - len('return;'))
            ev.append((pos, 2, 'tail', int(d.group(1), 16) if d else None))
        elif d:
            ev.append((pos, 2, 'call', int(d.group(1), 16)))
        elif TAILCALL_NAMES.match(name):
            ev.append((pos, 2, 'icall', None))
        elif name in FLAT_RUNTIME:
            ev.append((pos, 2, 'flat', None))
        else:
            ev.append((pos, 2, 'ind', None))
    for m in re.finditer(r'\breturn;', t):
        if m.start() not in consumed:
            ev.append((m.start(), 2, 'ret', None))
    for m in GOTO.finditer(t):
        ev.append((m.start(), 2, 'goto', int(m.group(1), 16)))
    for m in re.finditer(r'goto ' + LABEL_DISPATCH + ';', t):
        ev.append((m.start(), 2, 'dispatch', None))
    ev.sort(key=lambda e: (e[0], e[1]))
    return ev


def flag_flow(texts, succ, roots, falls, index, dispatch=None):
    """-> (obs, sites, isites, tails, itail). Sources of the value in F: 'E' the entry flags, 'P' a landing pad's,
    ('c', i, j) what the j-th direct call of statement i leaves, ('i', i, j) what its j-th call_indirect leaves, 'I' what
    a runtime call that may run guest code leaves. The state is a set of (source, via): via is the set of direct callees
    the value went through (each may leave the flags it was entered with, unless it KILLS). obs: source -> {(what, via)}:
    the observations that can see it before a definition replaces it (a definition under control flow replaces
    nothing). sites: [(i, j, g)] every direct non-tail call; isites: [(i, j)] every non-tail call_indirect; tails: the
    direct tail exit targets; itail: an indirect tail exit. dispatch: the statements an unresolved br may go to (None:
    any)."""
    n = len(texts)
    evs = [_flag_events(t) for t in texts]
    state = [None] * n
    obs = {}
    work = []
    none = frozenset()

    queued = [False] * n

    def push(i):
        if not queued[i]:
            queued[i] = True
            work.append(i)

    def join(i, s):
        if state[i] is None:
            state[i] = s
            push(i)
        elif not s <= state[i]:
            new = _norm(state[i] | s)
            if new != state[i]:
                state[i] = new
                push(i)

    def see(s, what, extra=None):
        for src, via in s:
            if extra is not None and len(via) < VIA_MAX:
                via = via | {extra}
            obs.setdefault(src, set()).add((what, via))
    join(roots[0], frozenset([('E', none)]))
    for r in roots[1:]:
        join(r, frozenset([('P', none)]))
    disp = none                                    # what an unresolved br brings to every statement it may go to
    while work:
        i = work.pop()
        queued[i] = False
        s, j, k = state[i], 0, 0
        if disp and (dispatch is None or i in dispatch):
            s = _norm(s | disp) if s is not None else disp
        if s is None:
            continue
        for _, _, kind, arg in evs[i]:
            if kind == 'read':
                see(s, 'R')
            elif kind == 'def':
                if arg:
                    s = none
            elif kind == 'call':                   # the callee may leave the flags it was entered with
                see(s, arg)
                s = _norm([(b, v | {arg} if len(v) < VIA_MAX else v) for b, v in s] + [(('c', i, j), none)])
                j += 1
            elif kind == 'icall':
                see(s, 'X')
                s, k = s | {(('i', i, k), none)}, k + 1
            elif kind == 'ind':
                see(s, 'X')
                s = s | {('I', none)}
            elif kind == 'tail':                   # the target may leave them for this function's caller
                see(s, 'X' if arg is None else arg)
                see(s, 'out', arg)
            elif kind == 'ret':
                see(s, 'out')
            elif kind == 'goto':
                if arg in index:
                    join(index[arg], s)
            elif kind == 'dispatch':               # a statement it may go to, or call_indirect and leave
                see(s, 'X')
                see(s, 'out')
                if not s <= disp:
                    disp = _norm(disp | s)
                    for x in (range(n) if dispatch is None else dispatch):
                        if x != i:
                            push(x)
        if falls[i]:
            join(i + 1, s)
    sites, isites, tails, itail = [], [], [], False
    for i, ev in enumerate(evs):
        j = k = 0
        for _, _, kind, arg in ev:
            if kind == 'call':
                sites.append((i, j, arg))
                j += 1
            elif kind == 'icall':
                isites.append((i, k))
                k += 1
            elif kind == 'tail':
                if arg is None:
                    itail = True
                else:
                    tails.append(arg)
            elif kind == 'dispatch':
                itail = True
    return dict((src, _norm_obs(o)) for src, o in obs.items()), sites, isites, tails, itail


def _norm_obs(o):
    """(what, via) pairs reduced as _norm does, per what"""
    return frozenset((w, v) for (w, v) in _norm(((w, v) for w, v in o)))


LITERAL = re.compile(r'\b0x([0-9a-f]{6,16})ULL\b')
# Not an address the code holds: the pc a function records (c->fn) and the site address a hook call passes (the hooks
# look it up; a hook runs guest code only on a saved and restored Ctx, see FLAT_RUNTIME).
SITE_ARG = re.compile(r'(?:\bc->fn = |\b(?:%s)\(c, )0x[0-9a-f]+ULL' % '|'.join(sorted(FLAT_RUNTIME)))


def summarize(stmts, pads=(), reach=None, keep=None, entry=None):
    """The flag summary of one function from its statements before the rewrite: (entry observations, ((g, observations
    of what that call leaves) per direct call), direct tail exit targets, observations of what code entered from outside
    the direct calls leaves, indirect tail exit, the address literals in its text (FlagTable taken), the vias by which
    the entry or a landing pad's flags reach an exit (KILLS)). reach: None, or the addresses an unresolved br of this
    function may go to inside it (held_targets()); None: any statement. keep: a dict that gets keep[entry] = what
    plan() needs of the flow (the observations of what each call leaves, the call sites)."""
    texts, index, succ, falls, roots = _cfg(stmts, pads)
    lits = frozenset(int(m.group(1), 16) for t in texts for m in LITERAL.finditer(SITE_ARG.sub('', t)))
    if any(escapes(t) and conditional_escape(t) for t in texts):
        return anything(texts, lits)
    dispatch = None if reach is None else frozenset(index[pc] for pc in reach if pc in index)
    obs, sites, isites, tails, itail = flag_flow(texts, succ, roots, falls, index, dispatch)
    if keep is not None:
        keep[entry] = (dict((k, v) for k, v in obs.items() if isinstance(k, tuple)), sites, isites)
    outside = set(obs.get('I', ())) | set(obs.get('P', ()))
    for i, k in isites:
        outside |= obs.get(('i', i, k), set())
    through = frozenset(v for src in ('E', 'P') for w, v in obs.get(src, ()) if w == 'out')
    return (obs.get('E', frozenset()), tuple((g, obs.get(('c', i, j), frozenset())) for i, j, g in sites),
            tuple(sorted(set(tails))), _norm_obs(outside), itail, lits, through)


HELD = 'held'


def held_targets(stmts, held):
    """The statements of a function an unresolved br may go to when its target takes no table arithmetic: only an
    address the program holds as a value can be one. held: the module's data words and relocation addends; with them
    the literals of the function's own text and every address in a 4 KiB page an adrp literal of it names (the other
    functions' literals: check_held())."""
    texts = [t for _, t in stmts]
    lits = {int(m.group(1), 16) for t in texts for m in LITERAL.finditer(SITE_ARG.sub('', t))}
    pages = {a for a in lits if a % 4096 == 0}
    return frozenset(pc for pc, _ in stmts if pc is not None and (pc in held or pc in lits or pc - pc % 4096 in pages))


def check_held(sums):
    """Takes the summaries[HELD, entry] notes out of sums (apply()) and puts back the summary without the dispatch limit
    for each function where another function's literal (or a 4 KiB page an adrp literal names) is one of the addresses
    the limit left out. -> the entries kept limited."""
    notes = {k[1]: sums.pop(k) for k in [k for k in sums if isinstance(k, tuple) and k[0] == HELD]}
    lits = set()
    for s in sums.values():
        if s != IMPORT:
            lits |= s[5]
    pages = {a for a in lits if a % 4096 == 0}
    kept = []
    for f, (others, whole) in sorted(notes.items()):
        if any(pc in lits or pc - pc % 4096 in pages for pc in others):
            sums[f] = whole
        else:
            kept.append(f)
    return kept


def anything(texts, lits=frozenset()):
    """The summary of a function whose flag flow is not followed: it reads its entry flags and what everything it calls
    leaves (every lifted function it names gets NEEDS_OUT), may return its entry flags, and reads what code reached from
    outside leaves if it calls any."""
    named, other = set(), False
    for t in texts:
        for m in CALL_C.finditer(t):
            name = m.group(1).split('::')[-1]
            d = DIRECT.match(name)
            if d:
                named.add(int(d.group(1), 16))
            elif not (name in SAFE or name in FLAT_RUNTIME or name in FLAG_DEF_NAMES or name in ('cond', 'get_nzcv')):
                other = True
    none = frozenset()
    R = frozenset([('R', none)])
    return (R | {('out', none)}, tuple((g, R) for g in sorted(named)), (), R if other else frozenset(), other, lits,
            frozenset([none]))


STUB = re.compile(r'^void f_[0-9a-f]+\(Ctx \*__restrict c\) \{ (?:f_([0-9a-f]+)\(c\)|(host_import)\(c, [^;]*\)); \}$')
IMPORT = 'import'
IMPORT_SUMMARY = (frozenset([('X', frozenset()), ('out', frozenset())]), (), (), frozenset(), False, frozenset(),
                  frozenset([frozenset()]))


def stub_summary(text):
    """The summary of a PLT stub: a tail call into this module, or IMPORT (a call into another module or the runtime:
    code entered from outside the direct calls); None for any other text."""
    m = STUB.match(text)
    if not m:
        return None
    if m.group(2):
        return IMPORT
    g = int(m.group(1), 16)
    return (frozenset([(g, frozenset()), ('out', frozenset([g]))]), (), (g,), frozenset(), False, frozenset(),
            frozenset([frozenset([g])]))


def _kills(sums):
    """KILLS(f): every way from f's entry (or a landing pad) to a return or tail exit defines the flags or goes through
    a callee that KILLS, so f never leaves the flags it was entered with. The greatest fixpoint: a way that goes through
    f itself again ends only through a way that does not."""
    kill = dict.fromkeys(sums, True)
    known = lambda g: kill.get(g, False)
    changed = True
    while changed:
        changed = False
        for f in sorted(sums):
            if kill[f] and any(not any(known(g) for g in via) for via in sums[f][6]):
                kill[f] = False
                changed = True
    return kill


def _fixpoint(sums, nout0, ind_in, kill):
    """NEEDS_IN, NEEDS_OUT: the least fixpoint (see above); nout0: the functions with NEEDS_OUT from the start."""
    nin = dict.fromkeys(sums, False)
    nout = {f: f in nout0 for f in sums}
    known_in = lambda g: nin.get(g, True)
    known_kill = lambda g: kill.get(g, False)
    order = sorted(sums)
    changed = True
    while changed:
        changed = False
        for f in order:
            entry, calls, tails = sums[f][:3]
            if not nin[f] and holds(entry, known_in, False, ind_in, known_kill):   # (at a return: the caller's flags)
                nin[f] = changed = True
            for g, cs in calls:
                if g in nout and not nout[g] and holds(cs, known_in, nout[f], ind_in, known_kill):
                    nout[g] = changed = True
            if nout[f]:
                for g in tails:
                    if g in nout and not nout[g]:
                        nout[g] = changed = True
    return nin, nout


def solve(sums, external=(), entered=()):
    """-> (FlagTable, report). sums: {entry: summarize() | IMPORT}; external: functions whose caller outside the lifted
    code reads their flags (NEEDS_OUT); entered: functions code outside the direct calls may enter: they get NEEDS_OUT
    (whoever enters them that way, in this module or another, may read what they leave) and are checked for NEEDS_IN
    (ind_in). report: (entered functions reading their entry flags (first pass), [(entry, what)] flags read that only a
    function this module does not lift can leave)."""
    imports = {f for f, s in sums.items() if s == IMPORT}
    sums = dict((f, IMPORT_SUMMARY if s == IMPORT else s) for f, s in sums.items())
    entered = set(entered) & set(sums)
    kill = _kills(sums)
    known_kill = lambda g: kill.get(g, False)
    ind_in = False
    nin, nout = _fixpoint(sums, set(external) | entered, ind_in, kill)
    bad_in = sorted(f for f in entered if nin[f] and f not in imports)
    if bad_in:                                     # code entered from outside reads its entry flags: publish for it
        ind_in = True
        nin, nout = _fixpoint(sums, set(external) | entered, ind_in, kill)
    known_in = lambda g: nin.get(g, True)
    foreign = []
    for f in sorted(sums):
        for g, cs in sums[f][1]:
            if g not in sums and holds(cs, known_in, nout[f], ind_in, known_kill):
                foreign.append((f, 'reads the flags of %x (not lifted here)' % g))
        if nout[f]:
            for g in sums[f][2]:
                if g not in sums:
                    foreign.append((f, 'tail exit to %x (not lifted here), and a caller reads its flags' % g))
    table = FlagTable((f, (nin[f], nout[f], kill[f])) for f in sorted(sums))
    table.ind_in = ind_in
    return table, (bad_in, foreign)


def live_after(succ, reads, writes, spills):
    """Per statement, the registers that may be read (or stored by a spill: spills[i]) after it before being written."""
    n = len(succ)
    preds = [[] for _ in range(n)]
    for i, s in enumerate(succ):
        for j in s:
            preds[j].append(i)
    live_in = [set() for _ in range(n)]
    work = list(range(n))
    while work:
        i = work.pop()
        lo = set().union(*[live_in[s] for s in succ[i]])
        li = reads[i] | spills[i] | (lo - writes[i])
        if li != live_in[i]:
            live_in[i] = li
            work.extend(preds[i])
    return [set().union(*[live_in[s] for s in succ[i]]) for i in range(n)]
