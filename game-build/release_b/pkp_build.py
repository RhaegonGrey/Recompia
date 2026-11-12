# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run game build of release model (b): turns the player's own dump into pkp.exe on their PC.

    python -I tools/build/pkp_build.py --exefs <dump>/updates/<v>/exefs --game <install folder> [--jobs N] [--keep]
    python -I tools/build/pkp_build.py --check --game <install folder>

The package ships no game code. This script
  1. verifies the dump's ExeFS against dump_hashes.json (the supported build: sha256 of every module + main's build ID),
  2. lifts the four NSOs with the bundled lifter (recomp/nxrecomp) into <game>/build/lift,
  3. compiles the generated C++ with the bundled Zig (zig c++, mingw target) into <game>/build/obj, in parallel,
  4. archives the objects into one library per module in <game>/build/lib (kept after the build),
  5. links pkp.exe = sdk/pkp_stub.cpp + the module libraries + sdk/pkp_rt.lib (the runtime, pkp_rt.dll in the package),
  6. deletes the generated sources and objects (unless --keep) and records the result in <game>/build/done.json.

Stamps, one per stage. The library stamp (lib_stamp(): the dump's main build ID and module hashes, the lifter sources,
the site lists, the module table, the locals mode, the compile flags, the compiler and the mods' hook plan hash) is everything
the module libraries are made from.
state.json holds it for the lift and obj folders of a build in progress, lib/stamp.json for the finished libraries.
A release that changes none of it (only the runtime or the SDK files) links again from the kept libraries: steps 2-4 are
skipped. Any other change, a missing or unreadable stamp, or a stamp from an older release (another FORMAT or shape)
lifts, compiles and links again (the old pkp.exe stays until the new one is linked over it).
Per module: module_stamp() is what one module's lift, objects and library are made from (its NSO, its site lists and the mod
hook sites generated for it, besides the lifter, flags and compiler). lib/modules.json keeps it with the libraries and
state.json with the lift and obj folders, so a changed library stamp lifts and compiles only the modules whose own stamp
changed (a plan with main-only points leaves sdk, subsdk0 and rtld alone); the others keep their library.
done.json is the full stamp (the dump plus tool_stamp(): everything in this package that goes into pkp.exe). --check
compares it with this package's tools only (no dump check, about a second) and prints one JSON line
{"current": bool, "reason": text, "relink": bool}; exit 0 current, 4 not but the kept libraries match (Set up only links), 3 not.
It also compares the hash of the mods' hook plan: another plan is never a relink.

Mod hook points: <game>/mods/.modtopia/hooks.tsv (the Modtopia plan the launcher writes before Set up; absent = none) lists stops
the mods need. nxrecomp/modplan.py checks each point against the dump and maps it to a site; the lift takes those sites with the
game's own site lists. A point that fails its checks is left out (and printed), never the whole build.
<game>/build/mod-hooks-built.tsv records the points pkp.exe has (written with pkp.exe; the runtime reads it). The plan's `# hash`
is in the library stamp, so another plan lifts and compiles again and the same plan keeps everything.

Object cache: every compiled object is also kept in <game>/build/objcache under a key of its source text, the generated headers
less the declarations it never names, the lifter's include/ tree, the compiler and the flags (class ObjCache), so Set up again
after a release that changes only some translation units compiles only those (a new hook site rewrites a few of main's units
and adds a declaration to funcs.h, which the other units do not name). It survives the stamp reset and the cleanup after
linking, is trimmed to --cache-mb (default 3072, 0 = off) and never serves a partial or damaged entry.

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Power cuts: every file a later run trusts without reading it again (a lifted module's sources, an object, a cache entry, a
library, pkp.exe) is flushed to the disk before the step that makes it trusted (flush()); otherwise a power cut can keep the
rename and the size but not the data, and an object read as zeros past that point failed the link on every Set up again.
Zig's own cache lasts one run, and an object whose end reads as zeros (an older release's, or a disk that dropped the flush)
is compiled again (whole_object()).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.join(HERE, 'recomp')
SDK = os.path.join(HERE, 'sdk')
DARWIN = sys.platform == 'darwin'
LINUX = sys.platform.startswith('linux')
# Windows: the bundled Zig. Linux (the native launcher): a Linux build of the same Zig version, which it fetches into
# tools/linux/zig (PKP_ZIG overrides it); it cross-compiles the same x86_64-windows-gnu objects
ZIG = (os.path.join(HERE, 'zig', 'zig.exe') if os.name == 'nt'
       else os.environ.get('PKP_ZIG') or os.path.join(os.path.dirname(HERE), 'linux', 'zig', 'zig'))
FORMAT = 3          # bump when the build layout or the stamp changes: older builds (partial or done) are then started over
# macOS: pkp's own program identity, the CFBundleIdentifier an Info.plist linked into the executable gives it (write_identity).
# Metal keeps compiled shaders in a cache per identity, and a binary without one shares its folder with every other program
# outside an app bundle. IDENTITY is in the build stamp: a change links pkp again.
APP_ID = 'org.pkp-recomp.game'
IDENTITY = 1
# the lifter as game-build/scripts/make-release.ps1 ships it: paths under RECOMP, the same in the zip (tools/build/recomp/...)
# and in the repo (recomp/...)
LIFTER = ['nxrecomp', 'include/nxrecomp', 'games/pkp/CMakeLists.txt', 'games/pkp/config.py']
# what the link takes from sdk/ besides the lifted objects and the runtime library (stamped on its own, as 'runtime')
if DARWIN:
    SDK_FILES = ['pkp_stub.cpp', 'pkp_rt_api.h', 'guest_segment.py']
elif LINUX:
    # Linux needs no resource script (pkp.rc/pkp.manifest are Windows) and no guest segment (the runtime reserves the range
    # itself with MAP_FIXED_NOREPLACE).
    SDK_FILES = ['pkp_stub.cpp', 'pkp_rt_api.h']
else:
    SDK_FILES = ['pkp_stub.cpp', 'pkp_rt_api.h', 'pkp.rc', 'pkp.manifest']

# the module table and the compile flags come from the bundled lifter, the same tables the developer build (CMake) reads:
# recomp/games/pkp/config.py (modules) and recomp/nxrecomp/compile_flags.py (ZIG_MINGW)
sys.path.insert(0, RECOMP)
from nxrecomp import compile_flags, modheader, modplan   # noqa: E402
from nxrecomp.config import load_game, reg_locals_mode   # noqa: E402
GAME = load_game('pkp')
# name, NSO file, guest base, NxModule.index, translation units
MODULES = [(m.name, m.source, m.base, i, m.shards) for i, m in enumerate(GAME.modules)]
# site lists per module, as paths under RECOMP (main's main.sites)
SITES = {m.name: 'games/pkp/' + m.sites for m in GAME.modules if m.sites}
if DARWIN:
    # macOS: Apple clang from the Command Line Tools (the package needs them, and the launcher says so when they are missing),
    # and the runtime as a dylib. The exe is Mach-O, so the guest segment is injected after the link.
    # Through xcrun: the Command Line Tools' clang++ invoked by its own path has no SDK sysroot, so the C++20 headers
    # (<bit>, <span>) are not found. xcrun supplies -isysroot.
    CLANGXX = ['xcrun', 'clang++']
    AR = shutil.which('ar') or 'ar'
    RUNTIME_LIB = 'libpkp_rt.dylib'
    TARGET_ARCH = 'arm64' if platform.machine() in ('arm64', 'aarch64') else platform.machine()
    CXXFLAGS = compile_flags.for_arch(TARGET_ARCH, 'clang') + ['-std=c++20']
    COMPILER = CLANGXX
elif LINUX:
    # Linux: the same bundled Zig, targeting x86_64-linux-gnu (so the package needs no system compiler), and the runtime as a
    # shared object the exe finds next to itself (-rpath $ORIGIN). No guest segment: the runtime reserves the guest ranges.
    RUNTIME_LIB = 'libpkp_rt.so'
    CXXFLAGS = compile_flags.ZIG_LINUX
    COMPILER = [ZIG, 'c++']
else:
    RUNTIME_LIB = 'pkp_rt.lib'
    CXXFLAGS = compile_flags.ZIG_MINGW
    COMPILER = [ZIG, 'c++']             # the compile command before the flags (tests put a fake one here)
MOD_PLAN = os.path.join('mods', '.modtopia', 'hooks.tsv')     # under <game>: the mods' hook plan (Modtopia "hooks 1")
MODULE_STAMPS = 'modules.json'          # in lib/: {module: module_stamp()} of the kept libraries
MOD_BUILT = 'mod-hooks-built.tsv'       # the points built in: in <game>/build/ for the runtime, in lib/ with the libraries
# guest registers and flags in C locals (recomp/nxrecomp/locals.py): the game config's reg_locals; PKP_LOCALS=off|exact|aapcs
# overrides it for a test build. In the stamps, so another mode lifts and compiles again
try:
    LOCALS = reg_locals_mode(GAME, os.environ.get('PKP_LOCALS'))
except ValueError as e:
    sys.exit('PKP_LOCALS: %s' % e)
CACHE_MB = 3072                         # default size cap of <game>/build/objcache
EXE = 'pkp' if (DARWIN or LINUX) else 'pkp.exe'    # the game executable the build links and --check looks for
# ObjCache: a one-line function declaration in a generated header ('void f_7100001000(Ctx *__restrict c);') and a C identifier
DECL_RE = re.compile(r'void\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*;\s*$')
NAME_RE = re.compile(r'[A-Za-z_]\w*')
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


def replace_file(src, dst):
    """Copy src over dst even when dst is read-only: a Homebrew dylib is 0444, and a player (or a test) may have put one
    there by hand, which made the copy fail with PermissionError and left the build half done."""
    try:
        os.remove(dst)
    except FileNotFoundError:
        pass
    except PermissionError:
        os.chmod(dst, 0o644)
        os.remove(dst)
    shutil.copy2(src, dst)


def _release_lock(path):
    try:
        os.remove(path)
    except OSError:
        pass


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def progress(phase, done, total, text):
    say('progress', phase, done, total, text)


class Fail(Exception):
    pass


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tree_hash(root, rels):
    """One sha256 over the files under root/rel for each rel (a file or a folder, walked in sorted order): each file's path
    relative to root and its sha256. Python's bytecode caches are left out (the lifter's own runs write them); a missing
    path counts as missing, so the hash still compares."""
    h = hashlib.sha256()
    for rel in rels:
        top = os.path.join(root, *rel.split('/'))
        if os.path.isfile(top):
            files = [rel]
        elif os.path.isdir(top):
            files = []
            for d, dirs, names in os.walk(top):
                dirs[:] = [x for x in dirs if x != '__pycache__']
                files += [os.path.relpath(os.path.join(d, n), root).replace('\\', '/') for n in names if not n.endswith('.pyc')]
        else:
            h.update(('%s\0missing\0' % rel).encode())
            continue
        for f in sorted(files):
            h.update(('%s\0%s\0' % (f, sha256(os.path.join(root, *f.split('/'))))).encode())
    return h.hexdigest()


def runtime_abi():
    """The ABI the executable links the runtime by, from sdk/pkp_rt_api.h. The library itself is not stamped: pkp loads it
    from its own folder (pkp_rt.dll, libpkp_rt.dylib), so a release that only fixes the runtime replaces that file and links
    nothing (the launcher copies it into the game folder before Play). Stamping the file made every such release ask for a
    full Set up."""
    try:
        with open(os.path.join(SDK, 'pkp_rt_api.h'), encoding='utf-8') as f:
            m = re.search(r'#define\s+PKP_RT_API\s+(\d+)', f.read())
        return 'api %s' % (m.group(1) if m else '?')
    except OSError:
        return 'api ?'


def tool_stamp():
    """The part of the build stamp this package decides: a release that changes any of it rebuilds pkp.exe."""
    return {'lifter': tree_hash(RECOMP, LIFTER), 'sites': tree_hash(RECOMP, sorted(SITES.values())), 'sdk': tree_hash(SDK, SDK_FILES),
            'runtime': runtime_abi(), 'cxxflags': CXXFLAGS, 'locals': LOCALS,
            'identity': IDENTITY if DARWIN else 0,
            'modules': [list(m) for m in MODULES],
            'modheader': hashlib.sha256(modheader.render(GAME.modules).encode()).hexdigest()}


def compiler_id():
    """The compiler for stamps and cache keys: the file hash of each part of COMPILER that is a file (the bundled Zig)."""
    return ','.join(sha256(c) if os.path.isfile(c) else c for c in COMPILER)


def lib_stamp(want, tools, compiler, mod_hooks):
    """Everything the module libraries are made from: the stamp of the lift, compile and archive stages. The runtime and
    the SDK files are not in it (they only go into the link). mod_hooks: the hash of the mods' hook plan ('' for none)."""
    stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'compiler': compiler,
             'mod_hooks': mod_hooks}
    stamp.update((k, tools[k]) for k in ('lifter', 'sites', 'cxxflags', 'locals', 'modules'))
    return stamp


def module_stamp(name, nso, want, tools, compiler, site_text):
    """Everything one module's lift, objects and library are made from: its own stamp, so a change that leaves a module's
    inputs alone keeps its lift and its library. The dump's build ID and this module's NSO hash, the lifter, the compile
    flags, the locals mode, the module table, the compiler and the lifter's include/ tree (the object cache key reads it),
    its own site list from the game and the text of the mod hook sites generated for it ('' for none). When unsure whether an
    input reaches a module, it belongs here: a stale lift is silently wrong code, an extra one only costs time."""
    return {'format': FORMAT, 'main_build_id': want['main_build_id'], 'nso': want['sha256'][nso], 'compiler': compiler,
            'lifter': tools['lifter'], 'include': tree_hash(RECOMP, ['include']), 'cxxflags': tools['cxxflags'],
            'locals': tools['locals'], 'modules': tools['modules'],
            'sites': tree_hash(RECOMP, [SITES[name]]) if name in SITES else '',
            'mod_sites': hashlib.sha256(site_text.encode('utf-8')).hexdigest() if site_text else ''}


def libs_current(lib, stamp):
    """Whether lib/ holds every module library and the record of their mod hook points, archived under exactly this stamp."""
    return read_json(os.path.join(lib, 'stamp.json')) == stamp and os.path.isfile(os.path.join(lib, MOD_BUILT)) and all(
        os.path.isfile(os.path.join(lib, 'pkp_%s.a' % m[0])) for m in MODULES)


def check(game):
    """Whether pkp.exe and <game>/build/done.json were built with this package's tools: (current, reason, relink).
    relink: not current, but the kept libraries match this package (the dump done.json names, lib_stamp()), so Set up only
    links again (minutes become seconds, and the launcher says so)."""
    if not os.path.isfile(os.path.join(game, EXE)):
        return False, '%s is not built yet' % EXE, False
    done = read_json(os.path.join(game, 'build', 'done.json'))
    if not isinstance(done, dict):
        return False, '%s has no finished build record' % EXE, False
    if done.get('format') != FORMAT or not isinstance(done.get('tools'), dict):
        return False, '%s was built by an older release' % EXE, False
    want = tool_stamp()
    changed = [k for k in want if done['tools'].get(k) != want[k]]
    plan_hash = modplan.read_hash(os.path.join(game, MOD_PLAN))
    if changed:
        # the kept libraries were archived with a plan's hash: only the current plan's make this a relink
        relink = libs_current(os.path.join(game, 'build', 'lib'), lib_stamp(done, want, compiler_id(), plan_hash))
        return False, 'this release changes ' + ', '.join(changed), relink
    if done.get('mod_hooks', '') != plan_hash:
        return False, "the mods' hook points changed", False
    return True, '%s matches this release' % EXE, False


def read_json(path):
    """The JSON in path, or None if it is missing or unreadable (a file cut short by a crash or power loss starts over)."""
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json(path, value):
    """Write through a temporary file and rename, so a stop mid-write never leaves half a file behind."""
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, indent=1)
    os.replace(tmp, path)


def flush(path):
    """Write path's data to the disk (FlushFileBuffers on Windows, fsync elsewhere). NTFS journals a rename and a file's size
    at once, its data later: after a power cut a file renamed a moment before can have its size and read as zeros."""
    with open(path, 'r+b') as f:
        os.fsync(f.fileno())


def flush_replace(tmp, path):
    """os.replace(tmp, path) once tmp's data is on the disk, so path is never there without it."""
    flush(tmp)
    os.replace(tmp, path)


def whole_object(path):
    """Whether the object file path is there and does not end in 4 KiB of zero bytes. A COFF object ends with its symbol and
    string tables, never zeros; a power cut that kept the size but not the data leaves zeros there (seen: the data stopped at
    4 MiB, the last 700 KiB were zero, and the link failed on every symbol of the unit)."""
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            f.seek(max(0, size - 4096))
            tail = f.read()
    except OSError:
        return False
    return size > 0 and tail.count(0) != len(tail)


def nso_short(path):
    """How many bytes an NSO lacks against the segments its header lists (0 = complete; a text-mode FTP copy drops a byte per CR LF pair).
    From pokopia-native (MIT), nxs2.py nso_short @7c3655d."""
    with open(path, 'rb') as f:
        h = f.read(0x70)
    if len(h) < 0x70 or h[:4] != b'NSO0':
        return 0
    flags = struct.unpack_from('<I', h, 0xC)[0]
    end = max(struct.unpack_from('<I', h, 0x10 + 0x10 * i)[0] + struct.unpack_from('<I', h, 0x60 + 4 * i if flags & (1 << i) else 0x18 + 0x10 * i)[0]
              for i in range(3))
    return max(0, end - os.path.getsize(path))


# main build ID (first 16 bytes) of the older game version; their sources disagree on its number, so player text says "an older version"
OLDER_MAIN_BUILD = '816c5358e8178d802fcec4a7ce5a741a'


def verify(exefs):
    want = json.load(open(os.path.join(HERE, 'dump_hashes.json'), encoding='utf-8'))
    missing = [n for n in want['sha256'] if not os.path.isfile(os.path.join(exefs, n))]
    if missing:
        raise Fail('The game files folder %s is missing %s. Pick the exefs of a complete dump.' % (exefs, ', '.join(missing)))
    with open(os.path.join(exefs, 'main'), 'rb') as f:
        head = f.read(0x60)
    if head[:4] != b'NSO0':
        raise Fail('exefs/main is not a decrypted NSO. Extract the game files with your own keys first (decrypted exefs and romfs).')
    bid = head[0x40:0x54].hex()
    if bid != want['main_build_id'] and bid.startswith(OLDER_MAIN_BUILD):
        raise Fail('These game files are from an older version. Update the game on your Switch 2 to %s, then dump again.' % want['version'].split()[0])
    if bid != want['main_build_id']:
        raise Fail('This is a different version of the game (main build ID %s). This release supports %s (build ID %s).'
                   % (bid, want['version'], want['main_build_id']))
    names = list(want['sha256'])
    for n in names:
        short = nso_short(os.path.join(exefs, n))
        if short:
            raise Fail('exefs/%s is %d bytes shorter than its own header says: the dump was copied in text mode, which damages it. '
                       'Copy the exefs folder again in binary mode (FileZilla: Transfer > Transfer type > Binary).' % (n, short))
    for i, n in enumerate(names):
        progress('verify', i, len(names), 'Checking your game files: %s' % n)
        got = sha256(os.path.join(exefs, n))
        if got != want['sha256'][n]:
            raise Fail('exefs/%s does not match the supported %s dump (sha256 %s). Re-dump the game, or check that the update %s is installed.'
                       % (n, want['version'], got, want['version']))
    progress('verify', len(names), len(names), 'Game files match %s' % want['version'])
    return want


def popen(cmd, **kw):
    # creation flags are Windows-only; on Linux the launcher lowered this script's priority, which its children inherit
    flags = CREATE_NO_WINDOW | BELOW_NORMAL if os.name == 'nt' else 0
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            creationflags=flags, **kw)


class Runner:
    """Runs tool processes; kills them all when the cancel file appears."""

    def __init__(self, cancel_file):
        self.cancel_file = cancel_file
        self.procs = set()
        self.lock = threading.Lock()
        self.cancelled = False
        threading.Thread(target=self._watch, daemon=True).start()

    def _watch(self):
        while True:
            if os.path.exists(self.cancel_file):
                self.cancelled = True
                with self.lock:
                    for p in list(self.procs):
                        try:
                            p.kill()
                        except OSError:
                            pass
            time.sleep(0.3)

    def check(self):
        if self.cancelled or os.path.exists(self.cancel_file):
            self.cancelled = True
            raise Cancelled()

    def run(self, cmd, env=None, cwd=None, on_line=None):
        self.check()
        p = popen(cmd, env=env, cwd=cwd)
        with self.lock:
            self.procs.add(p)
        tail = []
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            tail.append(line)
            del tail[:-30]
            if on_line:
                on_line(line)
        rc = p.wait()
        with self.lock:
            self.procs.discard(p)
        self.check()
        return rc, '\n'.join(tail)


def mod_sites(plan, exefs, gen):
    """The mods' hook points as site lists, in gen/mod-hooks/: <module>.sites (the sites to lift with besides the game's own) and
    MOD_BUILT (the points built in). Kept for a resumed build (the record is written last); returns {module: site list path}."""
    out = os.path.join(gen, 'mod-hooks')
    record = os.path.join(out, MOD_BUILT)
    if not os.path.isfile(record):
        os.makedirs(out, exist_ok=True)
        progress('lift', 0, len(MODULES), "Checking the mods' hook points")
        new, built, refused = {}, [], []
        if plan is not None:
            base = {n: [os.path.join(RECOMP, *p.split('/'))] for n, p in SITES.items()}
            new, built, refused = modplan.plan_sites(plan, GAME, exefs, base, log=lambda t: print('mod hooks: ' + t, flush=True))
        for line, text, reason in refused:
            print('mod hooks: line %d refused (%s): %s' % (line, text.replace('\t', ' '), reason), flush=True)
        for name, lines in new.items():
            with open(os.path.join(out, name + '.sites'), 'w', encoding='utf-8') as f:
                f.write('# the mods\' hook points, from <game>/mods/.modtopia/hooks.tsv\n' + '\n'.join(lines) + '\n')
            flush(os.path.join(out, name + '.sites'))
        tmp = record + '.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write(modplan.built_text(plan, built, GAME.version))
        flush_replace(tmp, record)
    return {m[0]: os.path.join(out, m[0] + '.sites') for m in MODULES if os.path.isfile(os.path.join(out, m[0] + '.sites'))}


def lift(runner, exefs, gen, jobs, extra_sites, skip=()):
    """One lifter process per module (the lifter itself runs `jobs` worker processes). extra_sites: {module: site list} to lift
    with besides the game's own (mod_sites()). skip: modules whose kept library is current (module_stamp()): not lifted."""
    for k, (name, nso, base, index, shards) in enumerate(MODULES):
        out = os.path.join(gen, name)
        if name in skip:
            continue
        marker = os.path.join(out, '.lifted')
        if os.path.isfile(marker):
            continue
        progress('lift', k, len(MODULES), 'Translating the game code: %s (%d of %d)' % (name, k + 1, len(MODULES)))
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(out)
        args = ['lift', os.path.join(exefs, nso), out, '--name', name, '--base', hex(base), '--shards', str(shards),
                '--jobs', str(jobs), '--game', 'pkp', '--index', str(index), '--locals', LOCALS]
        if name in SITES:
            args += ['--sites', os.path.join(RECOMP, *SITES[name].split('/'))]
        if name in extra_sites:
            args += ['--sites', extra_sites[name]]
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--lift-one', json.dumps(args)]
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
        for d, _, files in os.walk(out):     # the marker makes the sources trusted (flush())
            for n in files:
                flush(os.path.join(d, n))
        open(marker, 'w').write('ok\n')
    progress('lift', len(MODULES), len(MODULES), 'Game code translated')


def free_memory():
    """Available physical memory in bytes (Windows, Linux), or None."""
    if os.name != 'nt':
        try:
            with open('/proc/meminfo') as f:
                for line in f:
                    if line.startswith('MemAvailable:'):
                        return int(line.split()[1]) * 1024
        except (OSError, ValueError, IndexError):
            pass
        return None
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('len', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [(n, ctypes.c_uint64) for n in
                        ('total', 'avail', 'pagetotal', 'pageavail', 'virttotal', 'virtavail', 'extavail')]
        m = MS(len=ctypes.sizeof(MS))
        return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
    except (AttributeError, OSError):
        return None


class ObjCache:
    """Compiled objects kept by content, so a lifter or main.sites change recompiles only the translation units whose text changed.

    An entry is <key>-<size>.o in the cache folder. key = sha256 of the unit's source bytes, the part of the generated headers
    next to it that the unit can see (_header_view), the lifter's include/ tree, the compiler (its file hash) and the flags. The
    compiler or the flags changing gives other keys, so nothing stale is ever hit. An entry is written to a .tmp file, flushed
    and renamed, and a hit must have exactly the size in its name and be whole_object(), so a cut-off write or a damaged file
    is a miss (and is deleted).
    Hits are copied out, never linked. The folder is trimmed to the size cap, least recently used first (a hit refreshes the
    entry's time). Any cache error is a miss."""

    def __init__(self, folder, cap_bytes, compiler_id, flags_text, include_hash):
        self.folder = folder
        self.cap = cap_bytes
        self.base = '\0'.join([compiler_id, flags_text, include_hash])
        self.headers = {}                 # folder of the sources -> [(file name, other lines, {declared name: line})]
        self.lock = threading.Lock()
        self.hits = self.misses = 0
        os.makedirs(folder, exist_ok=True)

    def _headers(self, d):
        """The generated headers of a source folder, split into their function declarations (by name) and every other line."""
        with self.lock:
            if d not in self.headers:
                out = []
                for n in sorted(os.listdir(d)):
                    if n.endswith(('.h', '.hpp', '.inc')):
                        rest, decls = [], {}
                        with open(os.path.join(d, n), encoding='utf-8', newline='') as f:
                            for line in f:
                                m = DECL_RE.match(line)
                                if m:
                                    decls[m.group(1)] = line
                                else:
                                    rest.append(line)
                        out.append((n, ''.join(rest), decls))
                self.headers[d] = out
            return self.headers[d]

    def _header_view(self, src, text):
        """The headers as far as this unit is concerned: every line except the function declarations it never names.
        funcs.h declares every function of the module and every unit includes it, so one new function (a new midhook) would
        otherwise change the key of every unit of the module. A declaration the unit does not name cannot change its object:
        the lifter writes each guest call as the plain name (f_<addr>, u_<addr>, h_<name>), never pasted together in a macro."""
        headers = self._headers(os.path.dirname(src))
        named = set(NAME_RE.findall(text))
        for _, rest, _ in headers:      # a name the headers' other lines use (a macro, an inline) counts as named too
            named.update(NAME_RE.findall(rest))
        h = hashlib.sha256()
        for n, rest, decls in headers:
            h.update(('%s\0%s\0' % (n, rest)).encode())
            for name in sorted(named.intersection(decls)):
                h.update(decls[name].encode())
        return h.hexdigest()

    def key(self, src):
        with open(src, 'rb') as f:
            data = f.read()
        h = hashlib.sha256()
        h.update(self.base.encode())
        h.update(b'\0' + self._header_view(src, data.decode('utf-8', 'replace')).encode() + b'\0')
        h.update(hashlib.sha256(data).hexdigest().encode())
        return h.hexdigest()

    def _find(self, key):
        try:
            for n in os.listdir(self.folder):
                if n.startswith(key + '-') and n.endswith('.o'):
                    return n
        except OSError:
            pass
        return None

    def fetch(self, key, dst):
        """Copy the cached object for key to dst (through dst + '.tmp'); False on a miss or a bad entry."""
        n = self._find(key)
        if n:
            src = os.path.join(self.folder, n)
            tmp = dst + '.tmp'
            try:
                want = int(n[len(key) + 1:-2])
                if want > 0 and os.path.getsize(src) == want and whole_object(src):
                    shutil.copyfile(src, tmp)
                    if os.path.getsize(tmp) == want:
                        flush_replace(tmp, dst)
                        os.utime(src)
                        self.hits += 1
                        return True
                    os.remove(tmp)
                else:
                    os.remove(src)        # not the size it says, or its end is zeros: damaged, never used
            except (OSError, ValueError):
                pass
        self.misses += 1
        return False

    def store(self, key, obj):
        try:
            size = os.path.getsize(obj)
            if size <= 0:
                return
            final = os.path.join(self.folder, '%s-%d.o' % (key, size))
            tmp = final + '.%d.tmp' % threading.get_ident()
            shutil.copyfile(obj, tmp)
            if os.path.getsize(tmp) != size:
                os.remove(tmp)
                return
            flush_replace(tmp, final)
        except OSError:
            pass

    def trim(self):
        """Delete leftover .tmp files, then the least recently used entries until the folder fits the cap."""
        try:
            items = []
            for n in os.listdir(self.folder):
                p = os.path.join(self.folder, n)
                if n.endswith('.tmp'):
                    os.remove(p)
                elif n.endswith('.o'):
                    st = os.stat(p)
                    items.append((st.st_mtime, st.st_size, p))
            total = sum(i[1] for i in items)
            for _, size, p in sorted(items):
                if total <= self.cap:
                    break
                os.remove(p)
                total -= size
        except OSError:
            pass


def make_cache(folder, cap_mb, compiler):
    """The object cache for this package's compiler (compiler_id()) and flags, or None when cap_mb is 0."""
    if cap_mb <= 0:
        return None
    try:
        return ObjCache(folder, cap_mb << 20, compiler, ' '.join(CXXFLAGS), tree_hash(RECOMP, ['include']))
    except OSError:
        return None


def compile_all(runner, gen, obj, jobs, env, cache=None, skip=()):
    units = []
    for name, _, _, _, shards in MODULES:
        if name in skip:
            continue
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not whole_object(o):
            todo.append((name, src, o))
    done = total - len(todo)
    for name, _, _, _, _ in MODULES:
        if name not in skip:
            os.makedirs(os.path.join(obj, name), exist_ok=True)
    keys = {}
    if cache:
        rest = []
        for t in todo:
            keys[t[1]] = cache.key(t[1])
            if cache.fetch(keys[t[1]], t[2]):
                done += 1
            else:
                rest.append(t)
        todo = rest
    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    # biggest first, so the long ones do not end up last on a single core
    todo.sort(key=lambda t: -os.path.getsize(t[1]))

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        cmd = COMPILER + CXXFLAGS + ['-I', os.path.join(RECOMP, 'include'), '-I', os.path.dirname(src), '-c', src, '-o', tmp]
        rc, tail = runner.run(cmd, env=env)
        if rc != 0:
            return 'Compiling %s/%s failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail)
        flush_replace(tmp, o)
        if cache:
            cache.store(keys[src], o)
        return None

    # a big unit takes a GB or two in clang; with too many at once memory runs out and clang dies on every unit with
    # exit 541541187 (0x20474343: an uncaught C++ exception, std::bad_alloc)
    # ponytail: 2 GiB per job is a guess from the unit sizes; measure if machines with plenty of RAM get capped
    avail = free_memory()
    if avail:
        jobs = max(1, min(jobs, avail // (2 << 30)))
    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t): t for t in todo}
        try:
            while pending:
                fin, _ = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    t = pending.pop(f)
                    if f.result():
                        failed.append(t)
                        continue
                    done += 1
                    progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise
    for t in failed:                     # again one at a time with all the memory: only a failure here is real
        err = one(t)
        if err:
            raise Fail(err)
        done += 1
        progress('compile', done, total, 'Compiling: %d of %d' % (done, total))
    if cache:
        cache.trim()


def clear_high_entropy_va(exe):
    """pkp.exe keeps host allocations low (the guest ranges at 0x71_0000_0000 must stay free): no high-entropy ASLR,
    as /HIGHENTROPYVA:NO does for the clang-cl build. zig's linker has no flag for it, so clear the PE bit."""
    with open(exe, 'r+b') as f:
        f.seek(0x3c)
        pe = int.from_bytes(f.read(4), 'little')
        f.seek(pe + 24)
        if f.read(2) != b'\x0b\x02':
            raise Fail('pkp.exe is not a PE32+ image')
        off = pe + 24 + 70
        f.seek(off)
        dc = int.from_bytes(f.read(2), 'little') & ~0x20
        f.seek(off)
        f.write(dc.to_bytes(2, 'little'))


def archive(runner, obj, lib, stamp, env, record, mod_stamps, skip=()):
    """One library per module (lib/pkp_<name>.a) from the objects, except the modules in skip (their kept library is current),
    and the record of their mod hook points (record, copied to lib/MOD_BUILT), then lib/MODULE_STAMPS (each module's
    module_stamp()) and lib/stamp.json. The old stamps go first and the new ones are written last, so a stop or failure in
    between leaves libraries that are never taken as current."""
    os.makedirs(lib, exist_ok=True)
    for f in ('stamp.json', MODULE_STAMPS):
        if os.path.isfile(os.path.join(lib, f)):
            os.remove(os.path.join(lib, f))
    for name, _, _, _, _ in MODULES:
        if name in skip:
            continue
        a = os.path.join(lib, 'pkp_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        if DARWIN:
            # Apple's ar has no response files; a module's object list stays well inside ARG_MAX.
            rc, tail = runner.run([AR, 'rcs', a] + objs, env=env)
        else:
            rsp = os.path.join(obj, '%s.rsp' % name)
            # UTF-8, not the ANSI code page: zig ar reads the response file as UTF-8, so a game folder path with non-ASCII
            # characters (an accented user name) failed the archive step.
            with open(rsp, 'w', encoding='utf-8') as f:
                f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
            rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
        flush(a)                          # the stamps below make it trusted
    shutil.copyfile(record, os.path.join(lib, MOD_BUILT))
    flush(os.path.join(lib, MOD_BUILT))
    write_json(os.path.join(lib, MODULE_STAMPS), mod_stamps)
    write_json(os.path.join(lib, 'stamp.json'), stamp)


def write_identity(lib, game):
    """macOS: the Info.plist that gives pkp an identity of its own, returned as the path to link into __TEXT,__info_plist.

    Metal keeps compiled shaders in a cache per program identity: a folder of numbered files (functions.data, functions1.data ...)
    under DARWIN_USER_CACHE_DIR, where a process takes the first file no other running process of that identity holds and looks
    only in that file. A binary without an identity shares its folder with every other program outside an app bundle, so whenever
    one of those runs the game gets a different file than the one Set up filled and compiles every pipeline again while it loads
    (KosmicKrisp's VkPipelineCache keeps only MSL source, and MoltenVK's cache does not cover Metal's own compilation). The
    identifier is per game folder, so two installs never fight over one cache file."""
    ident = '%s.%s' % (APP_ID, hashlib.sha256(os.path.abspath(game).encode('utf-8')).hexdigest()[:12])
    path = os.path.join(lib, 'gen', 'Info.plist')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                '<plist version="1.0">\n<dict>\n'
                '    <key>CFBundleIdentifier</key><string>%s</string>\n'
                '    <key>CFBundleName</key><string>Pokopia Recomp</string>\n'
                '    <key>CFBundleExecutable</key><string>pkp</string>\n'
                '    <key>CFBundlePackageType</key><string>APPL</string>\n'
                '    <key>CFBundleShortVersionString</key><string>%s</string>\n'
                '</dict>\n</plist>\n' % (ident, os.environ.get('PKP_VERSION', '0')))
    return path


def link(runner, lib, game, env):
    libs = [os.path.join(lib, 'pkp_%s.a' % m[0]) for m in MODULES]
    exe = 'pkp' if (DARWIN or LINUX) else 'pkp.exe'
    tmp = os.path.join(lib, exe)
    modheader.write(GAME.modules, os.path.join(lib, 'gen', 'nx_modules.gen.h'))     # the module list pkp_stub.cpp includes
    if DARWIN:
        # Apple clang, the runtime dylib next to the exe (@rpath), and the guest segment in the Mach-O header afterwards.
        # __info_plist is the executable's own identity (write_identity): Metal's shader cache is keyed on it.
        cmd = list(CLANGXX) + CXXFLAGS + ['-I', SDK, '-I', os.path.join(lib, 'gen'), os.path.join(SDK, 'pkp_stub.cpp')] + libs + [
            os.path.join(SDK, RUNTIME_LIB), '-o', tmp, '-Wl,-stack_size,0x8000000', '-Wl,-rpath,@executable_path',
            '-Wl,-sectcreate,__TEXT,__info_plist,' + write_identity(lib, game),
            # guest_segment.py adds a load command for __GUEST after the link, which needs room in the header.
            '-Wl,-headerpad,0x4000']
        rc, tail = runner.run(cmd, env=env, cwd=SDK)
        if rc != 0:
            raise Fail('Linking %s failed (exit %d):\n%s' % (exe, rc, tail))
        rc, tail = runner.run([sys.executable, os.path.join(SDK, 'guest_segment.py'), tmp], env=env)
        if rc != 0:
            raise Fail('Reserving the guest address space failed (exit %d):\n%s' % (rc, tail))
    elif LINUX:
        # Zig targeting x86_64-linux-gnu. The exe finds libpkp_rt.so next to itself (-rpath $ORIGIN, the ELF equivalent of
        # @executable_path). The main thread's 128 MiB stack (the Windows /STACK, the Mach-O -stack_size) comes from the runtime raising
        # RLIMIT_STACK before main (os_posix.cpp raise_main_stack): glibc ignores the PT_GNU_STACK size below for the main thread.
        # --strip-debug: Zig's Linux output carries full DWARF (1.4 GB against 278 MB stripped); the symbol table stays, so the
        # crash reporter's host frames still resolve.
        cmd = list(COMPILER) + CXXFLAGS + ['-I', SDK, '-I', os.path.join(lib, 'gen'), os.path.join(SDK, 'pkp_stub.cpp')] + libs + [
            os.path.join(SDK, RUNTIME_LIB), '-o', tmp, '-Wl,-rpath,$ORIGIN', '-Wl,-z,stack-size=134217728', '-Wl,--strip-debug']
        rc, tail = runner.run(cmd, env=env, cwd=SDK)
        if rc != 0:
            raise Fail('Linking %s failed (exit %d):\n%s' % (exe, rc, tail))
    else:
        cmd = [ZIG, 'c++', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK, '-I', os.path.join(lib, 'gen'),
               os.path.join(SDK, 'pkp_stub.cpp'), os.path.join(SDK, 'pkp.rc')] + libs + [os.path.join(SDK, 'pkp_rt.lib'),
               '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
        rc, tail = runner.run(cmd, env=env, cwd=SDK)
        if rc != 0:
            raise Fail('Linking pkp.exe failed (exit %d):\n%s' % (rc, tail))
        clear_high_entropy_va(tmp)
    dst = os.path.join(game, exe)
    flush_replace(tmp, dst)                             # done.json then makes it trusted (check())
    if DARWIN or LINUX:
        # The exe loads the runtime from its own folder (-rpath @executable_path / $ORIGIN), as pkp.exe finds pkp_rt.dll beside it.
        replace_file(os.path.join(SDK, RUNTIME_LIB), os.path.join(game, RUNTIME_LIB))
        if DARWIN:
            # volk dlopen()s libvulkan.1.dylib from the same place: the package ships MoltenVK under that name. Linux uses the
            # system Vulkan loader and its ICDs, so there is nothing to copy.
            vulkan = os.path.join(SDK, 'libvulkan.1.dylib')
            if os.path.isfile(vulkan):
                replace_file(vulkan, os.path.join(game, 'libvulkan.1.dylib'))
        # What the game reads next to its executable: the keyboard and mouse prompt icons (pkp_kbm.cpp, without them the
        # prompts keep the game's own Switch sprites) and the shared font.
        # (names of their own: `dst` is the executable this function returns, and main hashes it)
        icons = os.path.join(SDK, 'kbm_icons')
        if os.path.isdir(icons):
            icons_dst = os.path.join(game, 'assets', 'kbm_icons')
            os.makedirs(icons_dst, exist_ok=True)
            for f in os.listdir(icons):
                if f.endswith('.png'):
                    replace_file(os.path.join(icons, f), os.path.join(icons_dst, f))
        font = os.path.join(SDK, 'fonts', 'DejaVuSans.ttf')
        if os.path.isfile(font):
            font_dst = os.path.join(game, 'fonts')
            os.makedirs(font_dst, exist_ok=True)
            replace_file(font, os.path.join(font_dst, 'DejaVuSans.ttf'))
    built = os.path.join(game, 'build', MOD_BUILT)      # the mod hook points this pkp.exe has, for the runtime
    shutil.copyfile(os.path.join(lib, MOD_BUILT), built + '.tmp')
    flush_replace(built + '.tmp', built)
    progress('link', 1, 1, '%s built' % exe)
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs')
    ap.add_argument('--game')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--cache-mb', type=int, default=CACHE_MB, help='size cap of the compiled-object cache in MiB (0 = off)')
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--check', action='store_true', help='only say whether <game>/build/done.json matches these tools')
    ap.add_argument('--lift-one', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.lift_one:                       # child process: one lifter run (keeps the lifter's own pool out of this process)
        from nxrecomp.__main__ import main as nx_main
        return nx_main(json.loads(a.lift_one)) or 0
    if a.check:
        if not a.game:
            ap.error('--check needs --game')
        try:
            current, reason, relink = check(os.path.abspath(a.game))
        except OSError as e:                 # a tool file that cannot be read: Set up then runs and names it
            current, reason, relink = False, 'cannot read the build tools: %s' % e, False
        print(json.dumps({'current': current, 'reason': reason, 'relink': relink}), flush=True)
        return 0 if current else 4 if relink else 3
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    # The lifter runs as a child process in another directory, so a relative --exefs would not resolve there (it failed with
    # FileNotFoundError in nxrecomp, after the game files had been verified).
    a.exefs = os.path.abspath(a.exefs)
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    # One build at a time per game folder: two used to wipe lift/ and obj/ under each other (the stamp check clears them),
    # which showed up as a compiler error on a source that had just been generated.
    lock_file = os.path.join(build, 'lock')
    if os.path.exists(lock_file):
        try:
            pid = int(open(lock_file).read().strip() or 0)
        except (OSError, ValueError):
            pid = 0
        if pid and pid != os.getpid():
            try:
                os.kill(pid, 0)          # a live build still holds it
                raise Fail('Another Set up is building this game folder already (process %d).' % pid)
            except ProcessLookupError:
                pass
            except PermissionError:
                raise Fail('Another Set up is building this game folder already (process %d).' % pid)
    open(lock_file, 'w').write(str(os.getpid()))
    t0 = time.time()
    times = {}
    try:
        # The link needs the runtime library from the package's sdk folder. A payload staged without it fails at the link,
        # after the lift and the compile: say so now instead.
        runtime_lib = os.path.join(SDK, RUNTIME_LIB)
        if not os.path.isfile(runtime_lib):
            raise Fail('The package is missing %s, which the link needs: extract the release zip again, or copy the runtime '
                       'library into %s.' % (RUNTIME_LIB, os.path.dirname(runtime_lib)))
        if DARWIN:
            if subprocess.run(['xcrun', '--find', 'clang++'], capture_output=True).returncode != 0:
                raise Fail('Apple clang was not found: run "xcode-select --install" and press Set up again.')
        elif not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        want = verify(a.exefs)
        tools = tool_stamp()
        plan = modplan.read_plan(os.path.join(game, MOD_PLAN))
        mod_hooks = plan.hash if plan else ''
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'tools': tools,
                 'mod_hooks': mod_hooks}
        compiler = compiler_id()
        libstamp = lib_stamp(want, tools, compiler, mod_hooks)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        if not DARWIN:
            # zig's cache lasts one run, as after linking: a stopped run's can hold an object a power cut left as zeros under a
            # manifest that survived, which zig hands back as compiled. The objects themselves are kept in obj/ and objcache/
            zig_cache = os.path.join(build, 'zig-cache')
            shutil.rmtree(zig_cache, ignore_errors=True)
            env['ZIG_GLOBAL_CACHE_DIR'] = zig_cache
            env['ZIG_LOCAL_CACHE_DIR'] = zig_cache
        gen, obj, lib = os.path.join(build, 'lift'), os.path.join(build, 'obj'), os.path.join(build, 'lib')
        reuse = libs_current(lib, libstamp)
        if reuse:                        # only the runtime or the SDK files changed: link the kept libraries again
            progress('lift', len(MODULES), len(MODULES), 'Game code unchanged')
            progress('compile', 1, 1, 'Game code unchanged')
        else:
            # the lift and obj folders belong to the modules' stamps in state.json: a module whose stamp changed (or with no
            # readable state, or one from an older release) starts over, the others keep their lift and objects. A module
            # whose kept library has its current stamp is not lifted or compiled at all
            state_p = os.path.join(build, 'state.json')
            state = read_json(state_p)
            state = state if isinstance(state, dict) else {}
            if state.get('mods') != libstamp:           # the generated mod-hooks folder is made from the whole library stamp
                shutil.rmtree(os.path.join(gen, 'mod-hooks'), ignore_errors=True)
            extra = mod_sites(plan, a.exefs, gen)
            mod_stamps = {}
            for name, nso, _, _, _ in MODULES:
                text = ''
                if name in extra:
                    with open(extra[name], encoding='utf-8', newline='') as f:
                        text = f.read()
                mod_stamps[name] = module_stamp(name, nso, want, tools, compiler, text)
            old = state.get('modules') if isinstance(state.get('modules'), dict) else {}
            for name in mod_stamps:
                if old.get(name) != mod_stamps[name]:
                    for d in (gen, obj):
                        shutil.rmtree(os.path.join(d, name), ignore_errors=True)
            write_json(state_p, {'mods': libstamp, 'modules': mod_stamps})
            kept = read_json(os.path.join(lib, MODULE_STAMPS))
            kept = kept if isinstance(kept, dict) else {}
            skip = {n for n in mod_stamps if kept.get(n) == mod_stamps[n] and os.path.isfile(os.path.join(lib, 'pkp_%s.a' % n))}
            for n in sorted(skip):
                print('Game code of %s unchanged: its kept library is used' % n, flush=True)
            t = time.time()
            lift(runner, a.exefs, gen, a.jobs, extra, skip)
            times['lift'] = round(time.time() - t, 1)
            t = time.time()
            compile_all(runner, gen, obj, a.jobs, env, make_cache(os.path.join(build, 'objcache'), a.cache_mb, compiler), skip)
            times['compile'] = round(time.time() - t, 1)
        t = time.time()
        progress('link', 0, 1, 'Linking %s' % EXE)
        if not reuse:
            archive(runner, obj, lib, libstamp, env, os.path.join(gen, 'mod-hooks', MOD_BUILT), mod_stamps, skip)
        try:
            exe = link(runner, lib, game, env)
        except Fail:
            if reuse:                    # a kept library may be damaged: the next Set up builds them again
                try:
                    os.remove(os.path.join(lib, 'stamp.json'))
                except OSError:
                    pass
            raise
        times['link'] = round(time.time() - t, 1)
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj', 'zig-cache'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        done = dict(stamp, exe_sha256=sha256(exe), seconds=times, finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        write_json(os.path.join(build, 'done.json'), done)
        progress('finish', 1, 1, '%s is ready (%d s)' % (EXE, time.time() - t0))
        _release_lock(lock_file)
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        _release_lock(lock_file)
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        _release_lock(lock_file)
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        _release_lock(lock_file)
        return 1


if __name__ == '__main__':
    sys.exit(main())
