# SPDX-License-Identifier: GPL-3.0-or-later
"""Mod loader hook plans -> site lists: the stops a Modtopia plan asks for, as lifter sites (sites.py).

A plan is the "modtopia hooks 1" file a Modtopia mod loader writes (`mt_plan_write`): comment lines, of which `# game <version>`
and `# hash <sha256>` are read, then one `module<TAB>offset<TAB>kind<TAB>word` line per point (offset and word in hex; one line
per address; kind observe / take_over / replace). Each point is a stop right before the instruction at module base + offset:
  observe    the handlers run, the code carries on                       -> inline
  take_over  the handlers may return from the function at once           -> midhook (entryhook at a function entry, where a
                                                                            midhook check is never compiled: emit_function)
  replace    the same at a function's entry, once per call               -> entryhook (must be a function entry the lifter knows)
A site the module's own site lists already have at that address serves a point when its kind does: inline serves observe, midhook
(not at a function entry) observe and take_over, entryhook take_over and replace. Then no site is added and the point uses that one.

Every point is checked on its own: the plan's game version against the config's, a known module, an address in its .text, the word
against the module's own (unpatched) code, and an entry for replace. A point that fails is refused (left out, with the reason) and
the others are still built. The record of what was built is the plan's format with a fifth column, the site kind that serves the
point (the runtime registers its hook there): `module offset kind word site`, under the plan's `# game` and `# hash` lines.
"""
import os

FORMAT_LINE = '# modtopia hooks 1'
KINDS = ('observe', 'take_over', 'replace')
SERVES = {'inline': ('observe',), 'midhook': ('observe', 'take_over'), 'entryhook': ('take_over', 'replace')}
HOOK_SITES = ('inline', 'midhook', 'entryhook')       # the order an existing site is picked in (the cheapest that serves)


class Point:
    def __init__(self, line, module, offset, kind, word):
        self.line, self.module, self.offset, self.kind, self.word = line, module, offset, kind, word

    def text(self):
        return '%s\t%#x\t%s\t%#010x' % (self.module, self.offset, self.kind, self.word)


class Plan:
    """A read plan: game version and hash ('' when the file has none), the points that parse, and the lines refused while reading
    ([(line number, text, reason)])."""

    def __init__(self, game='', hash_='', points=(), refused=()):
        self.game, self.hash, self.points, self.refused = game, hash_, list(points), list(refused)


def _header(lines):
    """(game, hash) from the comment lines at the top of a plan."""
    game = hash_ = ''
    for raw in lines:
        t = raw.strip()
        if t and not t.startswith('#'):
            break
        parts = t[1:].split(None, 1)
        if len(parts) == 2 and parts[0] in ('game', 'hash'):
            if parts[0] == 'game':
                game = parts[1].strip()
            else:
                hash_ = parts[1].strip()
    return game, hash_


def read_hash(path):
    """The plan's `# hash` ('' when the file is missing, unreadable or has none). Reads only the header: for quick checks."""
    try:
        with open(path, encoding='utf-8') as f:
            return _header(f)[1]
    except (OSError, UnicodeDecodeError):
        return ''


def read_plan(path):
    """The plan in path, or None when there is no such file. A file that is not a "modtopia hooks 1" plan has no points and one
    refused line saying so; a line that does not parse is refused on its own."""
    try:
        with open(path, encoding='utf-8') as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        return None
    except (OSError, UnicodeDecodeError) as e:
        return Plan(refused=[(0, path, 'cannot read the plan: %s' % e)])
    if not lines or lines[0].strip() != FORMAT_LINE:
        return Plan(refused=[(1, lines[0] if lines else '', 'not a "%s" plan' % FORMAT_LINE[2:])])
    game, hash_ = _header(lines)
    plan = Plan(game, hash_)
    seen = set()
    for ln, raw in enumerate(lines, 1):
        t = raw.strip()
        if not t or t.startswith('#'):
            continue
        f = t.split('\t')
        try:
            if len(f) != 4 or f[2] not in KINDS:
                raise ValueError('expected module, offset, kind (%s) and word' % ' / '.join(KINDS))
            p = Point(ln, f[0], int(f[1], 16), f[2], int(f[3], 16))
        except ValueError as e:
            plan.refused.append((ln, t, str(e)))
            continue
        if (p.module, p.offset) in seen:
            plan.refused.append((ln, t, 'a second point at the same address'))
            continue
        seen.add((p.module, p.offset))
        plan.points.append(p)
    return plan


def site_for(point, addr, env, entries):
    """The site kind that serves the point at guest address addr, and whether it is new (False: a site of env.sites already
    serves it). env: the module's Env with its own site lists loaded; entries: the function entries the lifter knows. Raises
    ValueError with the reason when the point cannot be built."""
    if not env.text_lo <= addr < env.text_hi or addr % 4:
        raise ValueError('not an instruction of %s' % env.img.name)
    have = env.img.word(addr)
    if have != point.word:
        raise ValueError('the code has %#010x there, the plan expects %#010x' % (have, point.word))
    entry = addr in entries
    for kind in HOOK_SITES:
        if addr in env.sites[kind] and point.kind in SERVES[kind] and not (kind == 'midhook' and entry):
            return kind, False
    if point.kind == 'replace':
        if not entry:
            raise ValueError('replace needs a function entry, and this is not one')
        return 'entryhook', True
    if point.kind == 'take_over':
        return ('entryhook' if entry else 'midhook'), True
    return 'inline', True


def plan_sites(plan, cfg, exefs, base_sites, log=print):
    """Map the plan's points onto sites. cfg: the GameConfig (its modules give the names, bases and files in exefs); base_sites:
    {module name: [site list paths]} the module is lifted with anyway. Returns (new, built, refused):
      new      {module name: [site list lines]} the sites to lift with in addition (only modules that get some)
      built    [(Point, site kind)] the points built in, in plan order
      refused  [(line number, text, reason)] the plan's refused lines plus the points refused here"""
    from .driver import open_image
    from .env import Env
    from .sites import load_sites
    new, built, refused = {}, [], list(plan.refused)
    version = getattr(cfg, 'version', None)
    if version and plan.game != version:
        refused += [(p.line, p.text(), 'the plan is for game %s, this is %s' % (plan.game or '?', version)) for p in plan.points]
        return new, built, refused
    specs = {m.name: m for m in cfg.modules}
    by_module = {}
    for p in plan.points:
        if p.module in specs:
            by_module.setdefault(p.module, []).append(p)
        else:
            refused.append((p.line, p.text(), 'no module %s in this game' % p.module))
    for name, points in by_module.items():
        m = specs[name]
        img = open_image({'kind': m.kind, 'path': os.path.join(exefs, m.source or m.name), 'base': m.base, 'name': m.name})
        env = Env(img, cfg)
        if base_sites.get(name):
            load_sites(base_sites[name], env)
        entries = set(env.compute_starts())
        for p in points:
            addr = m.base + p.offset
            try:
                kind, fresh = site_for(p, addr, env, entries)
            except ValueError as e:
                refused.append((p.line, p.text(), str(e)))
                continue
            if fresh:
                env.sites[kind].add(addr)
                new.setdefault(name, []).append('%s %#x  # %s' % (kind, addr, p.text().replace('\t', ' ')))
            built.append((p, kind))
        log('%s: %d mod hook points built (%d new sites)' % (name, sum(1 for p, _ in built if p.module == name), len(new.get(name, ()))))
    built.sort(key=lambda b: b[0].line)
    refused.sort()
    return new, built, refused


def built_text(plan, built, game=''):
    """The record of the points built in: the plan's header lines, then `module offset kind word site` per point."""
    out = [FORMAT_LINE, '# game %s' % (plan.game if plan else game), '# hash %s' % (plan.hash if plan else ''),
           '# module\toffset\tkind\tword\tsite']
    out += ['%s\t%s' % (p.text(), kind) for p, kind in built]
    return '\n'.join(out) + '\n'
