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
the site lists, the module table, the locals mode, the compile flags and the compiler) is everything the module libraries
are made from.
state.json holds it for the lift and obj folders of a build in progress, lib/stamp.json for the finished libraries.
A release that changes none of it (only the runtime or the SDK files) links again from the kept libraries: steps 2-4 are
skipped. Any other change, a missing or unreadable stamp, or a stamp from an older release (another FORMAT or shape)
lifts, compiles and links again from scratch (the old pkp.exe stays until the new one is linked over it).
done.json is the full stamp (the dump plus tool_stamp(): everything in this package that goes into pkp.exe). --check
compares it with this package's tools only (no dump check, well under a second) and prints one JSON line
{"current": bool, "reason": text}; exit 0 current, 3 not.

Object cache: every compiled object is also kept in <game>/build/objcache under a key of its source text, the generated headers,
the lifter's include/ tree, the compiler and the flags (class ObjCache), so Set up again after a release that changes only some
translation units compiles only those. It survives the stamp reset and the cleanup after linking, is trimmed to --cache-mb
(default 3072, 0 = off) and never serves a partial or damaged entry.

Resumable: a module's lift counts once its marker file is written, an object once it is renamed into place, so a stopped
build continues where it stopped. A file named <game>/build/cancel stops it (the launcher's Pause).
Output for the launcher, one per line on stdout:  @progress <phase> <done> <total> <text>  /  @error <text>
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
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
# Windows: the bundled Zig. Linux (the native launcher): a Linux build of the same Zig version, which it fetches into
# tools/linux/zig (PKP_ZIG overrides it); it cross-compiles the same x86_64-windows-gnu objects
ZIG = (os.path.join(HERE, 'zig', 'zig.exe') if os.name == 'nt'
       else os.environ.get('PKP_ZIG') or os.path.join(os.path.dirname(HERE), 'linux', 'zig', 'zig'))
FORMAT = 3          # bump when the build layout or the stamp changes: older builds (partial or done) are then started over
# the lifter as game-build/scripts/make-release.ps1 ships it: paths under RECOMP, the same in the zip (tools/build/recomp/...)
# and in the repo (recomp/...)
LIFTER = ['nxrecomp', 'include/nxrecomp', 'games/pkp/CMakeLists.txt', 'games/pkp/config.py']
# what the link takes from sdk/ besides the lifted objects and pkp_rt.lib (stamped on its own, as 'runtime')
SDK_FILES = ['pkp_stub.cpp', 'pkp_rt_api.h', 'pkp.rc', 'pkp.manifest']

# the module table and the compile flags come from the bundled lifter, the same tables the developer build (CMake) reads:
# recomp/games/pkp/config.py (modules) and recomp/nxrecomp/compile_flags.py (ZIG_MINGW)
sys.path.insert(0, RECOMP)
from nxrecomp import compile_flags, modheader   # noqa: E402
from nxrecomp.config import load_game, reg_locals_mode   # noqa: E402
GAME = load_game('pkp')
# name, NSO file, guest base, NxModule.index, translation units
MODULES = [(m.name, m.source, m.base, i, m.shards) for i, m in enumerate(GAME.modules)]
# site lists per module, as paths under RECOMP (main's main.sites)
SITES = {m.name: 'games/pkp/' + m.sites for m in GAME.modules if m.sites}
CXXFLAGS = compile_flags.ZIG_MINGW
# guest registers and flags in C locals (recomp/nxrecomp/locals.py): the game config's reg_locals; PKP_LOCALS=off|exact|aapcs
# overrides it for a test build. In the stamps, so another mode lifts and compiles again
try:
    LOCALS = reg_locals_mode(GAME, os.environ.get('PKP_LOCALS'))
except ValueError as e:
    sys.exit('PKP_LOCALS: %s' % e)
COMPILER = [ZIG, 'c++']                 # the compile command before the flags (tests put a fake one here)
CACHE_MB = 3072                         # default size cap of <game>/build/objcache
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000


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


def tool_stamp():
    """The part of the build stamp this package decides: a release that changes any of it rebuilds pkp.exe."""
    return {'lifter': tree_hash(RECOMP, LIFTER), 'sites': tree_hash(RECOMP, sorted(SITES.values())), 'sdk': tree_hash(SDK, SDK_FILES),
            'runtime': sha256(os.path.join(SDK, 'pkp_rt.lib')), 'cxxflags': CXXFLAGS, 'locals': LOCALS,
            'modules': [list(m) for m in MODULES],
            'modheader': hashlib.sha256(modheader.render(GAME.modules).encode()).hexdigest()}


def compiler_id():
    """The compiler for stamps and cache keys: the file hash of each part of COMPILER that is a file (the bundled Zig)."""
    return ','.join(sha256(c) if os.path.isfile(c) else c for c in COMPILER)


def lib_stamp(want, tools, compiler):
    """Everything the module libraries are made from: the stamp of the lift, compile and archive stages. The runtime and
    the SDK files are not in it (they only go into the link)."""
    stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'compiler': compiler}
    stamp.update((k, tools[k]) for k in ('lifter', 'sites', 'cxxflags', 'locals', 'modules'))
    return stamp


def libs_current(lib, stamp):
    """Whether lib/ holds every module library, archived under exactly this stamp."""
    return read_json(os.path.join(lib, 'stamp.json')) == stamp and all(
        os.path.isfile(os.path.join(lib, 'pkp_%s.a' % m[0])) for m in MODULES)


def check(game):
    """Whether pkp.exe and <game>/build/done.json were built with this package's tools: (current, reason)."""
    if not os.path.isfile(os.path.join(game, 'pkp.exe')):
        return False, 'pkp.exe is not built yet'
    done = read_json(os.path.join(game, 'build', 'done.json'))
    if not isinstance(done, dict):
        return False, 'pkp.exe has no finished build record'
    if done.get('format') != FORMAT or not isinstance(done.get('tools'), dict):
        return False, 'pkp.exe was built by an older release'
    want = tool_stamp()
    changed = [k for k in want if done['tools'].get(k) != want[k]]
    if changed:
        return False, 'this release changes ' + ', '.join(changed)
    return True, 'pkp.exe matches this release'


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


def lift(runner, exefs, gen, jobs):
    """One lifter process per module (the lifter itself runs `jobs` worker processes)."""
    for k, (name, nso, base, index, shards) in enumerate(MODULES):
        out = os.path.join(gen, name)
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
        cmd = [sys.executable, '-I', os.path.abspath(__file__), '--lift-one', json.dumps(args)]
        rc, tail = runner.run(cmd, cwd=RECOMP)
        if rc != 0:
            raise Fail('Translating %s failed (exit %d):\n%s' % (name, rc, tail))
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

    An entry is <key>-<size>.o in the cache folder. key = sha256 of the unit's source bytes, the generated headers next to it,
    the lifter's include/ tree, the compiler (its file hash) and the flags. The compiler or the flags changing gives other keys,
    so nothing stale is ever hit. An entry is written to a .tmp file and renamed, and a hit must have exactly the size in its
    name, so a cut-off write or a damaged file is a miss (and is deleted). Hits are copied out, never linked. The folder is
    trimmed to the size cap, least recently used first (a hit refreshes the entry's time). Any cache error is a miss."""

    def __init__(self, folder, cap_bytes, compiler_id, flags_text, include_hash):
        self.folder = folder
        self.cap = cap_bytes
        self.base = '\0'.join([compiler_id, flags_text, include_hash])
        self.headers = {}                 # folder of the sources -> hash of the .h files in it
        self.lock = threading.Lock()
        self.hits = self.misses = 0
        os.makedirs(folder, exist_ok=True)

    def _headers(self, d):
        with self.lock:
            if d not in self.headers:
                h = hashlib.sha256()
                for n in sorted(os.listdir(d)):
                    if n.endswith(('.h', '.hpp', '.inc')):
                        h.update(('%s\0%s\0' % (n, sha256(os.path.join(d, n)))).encode())
                self.headers[d] = h.hexdigest()
            return self.headers[d]

    def key(self, src):
        h = hashlib.sha256()
        h.update(self.base.encode())
        h.update(b'\0' + self._headers(os.path.dirname(src)).encode() + b'\0')
        h.update(sha256(src).encode())
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
                if want > 0 and os.path.getsize(src) == want:
                    shutil.copyfile(src, tmp)
                    if os.path.getsize(tmp) == want:
                        os.replace(tmp, dst)
                        os.utime(src)
                        self.hits += 1
                        return True
                    os.remove(tmp)
                else:
                    os.remove(src)        # not the size it says: damaged, never used
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
            os.replace(tmp, final)
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


def compile_all(runner, gen, obj, jobs, env, cache=None):
    units = []
    for name, _, _, _, shards in MODULES:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src)[:-4] + '.o')
        if not os.path.isfile(o):
            todo.append((name, src, o))
    done = total - len(todo)
    for name, _, _, _, _ in MODULES:
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
        os.replace(tmp, o)
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


def archive(runner, obj, lib, stamp, env):
    """One library per module (lib/pkp_<name>.a) from the objects, then lib/stamp.json. The old stamp goes first and the
    new one is written last, so a stop or failure in between leaves libraries that are never taken as current."""
    os.makedirs(lib, exist_ok=True)
    stamp_p = os.path.join(lib, 'stamp.json')
    if os.path.isfile(stamp_p):
        os.remove(stamp_p)
    for name, _, _, _, _ in MODULES:
        a = os.path.join(lib, 'pkp_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(os.path.join(obj, name, f) for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        # UTF-8, not the ANSI code page: zig ar reads the response file as UTF-8, so a game folder path with non-ASCII
        # characters (an accented user name) failed the archive step.
        with open(rsp, 'w', encoding='utf-8') as f:
            f.write('\n'.join('"%s"' % p.replace('\\', '/') for p in objs))
        rc, tail = runner.run([ZIG, 'ar', 'rcs', a, '@' + rsp], env=env)
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
    write_json(stamp_p, stamp)


def link(runner, lib, game, env):
    libs = [os.path.join(lib, 'pkp_%s.a' % m[0]) for m in MODULES]
    tmp = os.path.join(lib, 'pkp.exe')
    modheader.write(GAME.modules, os.path.join(lib, 'gen', 'nx_modules.gen.h'))     # the module list pkp_stub.cpp includes
    cmd = [ZIG, 'c++', '-target', 'x86_64-windows-gnu', '-mcpu=x86_64+cx16', '-O1', '-fno-exceptions', '-fno-rtti', '-I', SDK, '-I', os.path.join(lib, 'gen'),
           os.path.join(SDK, 'pkp_stub.cpp'), os.path.join(SDK, 'pkp.rc')] + libs + [os.path.join(SDK, 'pkp_rt.lib'),
           '-o', tmp, '-Wl,--stack,134217728', '-Wl,--subsystem,windows']
    rc, tail = runner.run(cmd, env=env, cwd=SDK)
    if rc != 0:
        raise Fail('Linking pkp.exe failed (exit %d):\n%s' % (rc, tail))
    clear_high_entropy_va(tmp)
    dst = os.path.join(game, 'pkp.exe')
    os.replace(tmp, dst)
    progress('link', 1, 1, 'pkp.exe built')
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
            current, reason = check(os.path.abspath(a.game))
        except OSError as e:                 # a tool file that cannot be read: Set up then runs and names it
            current, reason = False, 'cannot read the build tools: %s' % e
        print(json.dumps({'current': current, 'reason': reason}), flush=True)
        return 0 if current else 3
    if not a.exefs or not a.game:
        ap.error('--exefs and --game are required')
    game = os.path.abspath(a.game)
    build = os.path.join(game, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        if not os.path.isfile(ZIG):
            raise Fail('The bundled compiler is missing (%s). Extract the release zip again.' % ZIG)
        want = verify(a.exefs)
        tools = tool_stamp()
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'], 'tools': tools}
        compiler = compiler_id()
        libstamp = lib_stamp(want, tools, compiler)
        state_p = os.path.join(build, 'state.json')
        # the lift and obj folders belong to the library stamp in state.json: another dump, lifter, flags or compiler (or no
        # readable state, or one from an older release) starts them over. Removing lift/ drops the modules' .lifted markers
        # too, so a changed lifter or main.sites lifts everything again, not only recompiles
        if read_json(state_p) != libstamp:
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            write_json(state_p, libstamp)
        times['verify'] = round(time.time() - t0, 1)
        runner = Runner(cancel_file)
        env = dict(os.environ)
        env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(build, 'zig-cache')
        gen, obj, lib = os.path.join(build, 'lift'), os.path.join(build, 'obj'), os.path.join(build, 'lib')
        reuse = libs_current(lib, libstamp)
        if reuse:                        # only the runtime or the SDK files changed: link the kept libraries again
            progress('lift', len(MODULES), len(MODULES), 'Game code unchanged')
            progress('compile', 1, 1, 'Game code unchanged')
        else:
            t = time.time()
            lift(runner, a.exefs, gen, a.jobs)
            times['lift'] = round(time.time() - t, 1)
            t = time.time()
            compile_all(runner, gen, obj, a.jobs, env, make_cache(os.path.join(build, 'objcache'), a.cache_mb, compiler))
            times['compile'] = round(time.time() - t, 1)
        t = time.time()
        progress('link', 0, 1, 'Linking pkp.exe')
        if not reuse:
            archive(runner, obj, lib, libstamp, env)
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
        progress('finish', 1, 1, 'pkp.exe is ready (%d s)' % (time.time() - t0))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Set up again continues where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:            # an unexpected failure: say what it was, plainly
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
