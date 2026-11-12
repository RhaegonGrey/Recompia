# SPDX-License-Identifier: GPL-3.0-or-later
"""Instruction patches and site lists: the run-time code changes compiled into the lifted code (parsers only)."""


# ---------------------------------------------------------------------------------------------------- patches
def load_patches(path, img):
    """Instruction patches: one `<address> <original word> <new word>  # note` per line (hex). The original word is
    checked against the image. Returns {address: new word}."""
    out = {}
    with open(path) as f:
        for line in f:
            t = line.split('#', 1)[0].split()
            if not t:
                continue
            addr, old, new = int(t[0], 16), int(t[1], 16), int(t[2], 16)
            have = img.word(addr)
            if have != old:
                raise ValueError('patch %x: the binary has %08x, the patch expects %08x' % (addr, have, old))
            out[addr] = new
    return out


# ---------------------------------------------------------------------------------------------------- sites
SITE_KINDS = ('patch', 'inline', 'midhook', 'dynimm', 'jtable', 'noinline', 'entryhook')


def load_sites(paths, env):
    """Site lists: run-time code changes the runtime / mods make, compiled into the lifted code. `paths` is one file or a list of
    files (lift --sites, repeatable), merged by address in the order given. One site per line, `#` starts a
    comment, addresses are full guest addresses in hex (after ssbu-recomp tools/lifter/relift_sites.py, GPL-3.0-or-later):
      patch    <addr> <orig word> <new word>   the instruction word is replaced before lifting (orig is checked; several
                                               writes to one address chain: each orig = the previous new)
      inline   <addr>   `nx_site_inline(c, addr);` before the instruction (all registers are in c)
      midhook  <addr>   `if (nx_site_midhook(c, addr)) return;` before the instruction, and a new lifted function f_<addr>
                        starting there: the runtime's "original" to continue with
      entryhook <addr>  `if (nx_hook_entry(c, addr)) return;` at the start of the function (a function hook); the function is
                        noinline so no caller in its translation unit inlines the check away
      dynimm   <addr>   the immediate of the add/sub-immediate or movz/movn at addr is read from guest memory each time it runs
      noinline <addr>   the function entry is compiled `noinline`
      jtable   <addr>   a `br` whose table is patched at run time: checked to be a `br`, nothing else (the lifter reads
                        table entries from guest memory when it dispatches)
    Merging: one site per address and kind, however many lists name it. Different kinds at one address are all kept, each
    compiled as above (a function hook first, then the inline callout, then the midhook check, which is never compiled at a
    function's entry: emit_function), because the runtime keeps one hook chain per kind: dropping one kind would silently skip
    the hooks registered for it. Patches chain across
    the lists as within one (each orig = the previous new, in list order), so two lists that patch one word differently are
    refused, naming both lines.
    Fills env.sites[kind] (sets of addresses) and env.patches."""
    if isinstance(paths, str):
        paths = [paths]
    env.sites = {k: set() for k in SITE_KINDS}
    chain = {}                                    # address -> (word after the patches so far, where the last one is)
    for path in paths:
        with open(path) as f:
            for ln, line in enumerate(f, 1):
                t = line.split('#', 1)[0].split()
                if not t:
                    continue
                kind, addr = t[0], int(t[1], 16)
                if kind not in SITE_KINDS:
                    raise ValueError('%s:%d: unknown site kind %s' % (path, ln, kind))
                if kind == 'patch':
                    old, new = int(t[2], 16), int(t[3], 16)
                    have, where = chain.get(addr, (env.img.word(addr), None))
                    if have != old:
                        raise ValueError('%s:%d: patch %x: the code has %08x%s, the patch expects %08x'
                                         % (path, ln, addr, have, ' after the patch at ' + where if where else '', old))
                    chain[addr] = (new, '%s:%d' % (path, ln))
                    env.patches[addr] = new
                else:
                    env.sites[kind].add(addr)
                    if kind == 'jtable' and (env.word(addr) & 0xfffffc1f) != 0xd61f0000:
                        raise ValueError('%s:%d: jtable %x is not a br' % (path, ln, addr))

