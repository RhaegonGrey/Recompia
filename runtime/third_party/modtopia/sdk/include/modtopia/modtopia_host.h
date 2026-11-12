/* Modtopia core: what a host (Pokopia Native, Recompia, ...) calls, and the small backend it provides. MIT.
 *
 * A host is a game build that runs Modtopia mods. It links this core (core/src, C11, no dependencies) or implements the same behaviour, and
 * provides an MtBackend: guest memory, registers at a hook stop, guest calls, its module bases, which hook points its game has built in, and its
 * game files. Everything else (finding mods, unpacking, manifests, versions, dependencies, order, switches, settings, the hook plan, the data
 * overlay's folders, loading code, the MtHost table mods see, the event bus, hook dispatch) is the core's.
 *
 * Every function's contract is written beside it. docs/HOST.md walks through a host; tests/conformance/ is the check a host must pass (the
 * example mods on the real game, with the log lines and effects expected).
 *
 * The order of calls:
 *
 *     MtLoader *L = mt_loader_new(&opt, &backend);    before the game starts
 *     mt_loader_scan(L);                               discover, unpack, resolve (a launcher's "plan" stops here, with no game)
 *     for i < mt_loader_overlay_count(L):              lay mt_loader_overlay_root(L, i) over the game's RomFS, index 0 first
 *     mt_loader_start(L);                              check the points against the game, load the code mods, call their entry points
 *     ... the game boots ...
 *     if (mt_loader_apply_patches(L) > 0)              once the game's files can be read (game_file_size, game_read), before the game
 *         lay the overlay again;                         reads them: the list now has the folders of patched game files
 *     mt_loader_check_data(L);                         once the game's files can be asked about (game_has_file)
 *     ... the game runs ...
 *     at a built-in hook point:   if (mt_hook_dispatch(L, site, host_ctx) == MT_HANDLED) return from the game function;
 *     once a game frame:          mt_loader_frame(L);              on the game's thread
 *     after the game commits its save: mt_loader_save_written(L);
 *
 * Threads: mt_hook_dispatch is safe on any number of game threads at once and takes no lock. mt_loader_frame and mt_loader_save_written are
 * called by one thread at a time. The mod-list and settings calls are for one thread at a time (a menu), which may differ from the game's.
 * Strings the core returns stay valid until the next call that changes the same mod, unless said otherwise.
 */
#ifndef MODTOPIA_HOST_H
#define MODTOPIA_HOST_H

#include "modtopia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT_CORE_VERSION "0.6.1"

typedef struct MtLoader MtLoader;

/* ---------------------------------------------------------------- what the host provides
 * Every callback gets `user` back as given. Results use the SDK's MT_OK / MT_ERR_* codes. A callback may be NULL where it says so; the core then
 * does without it (a launcher's plan has no running game: only size, the names and log are set). */
typedef struct MtBackend {
    uint32_t size;               /* sizeof(MtBackend) as the host was compiled: fields beyond it count as NULL */
    void *user;
    const char *host_name;       /* shown to mods (MtHost.host_name) and in logs, e.g. "Pokopia Native 1.5.0" */
    const char *game_version;    /* "2.0.1", "1.0.5": picks each mod's [[hooks."<version>"]] set and the mappings file. NULL: unknown (mods with
                                    hook points stay off; version checks are skipped) */
    const char *game_build_id;   /* main's build ID, hex. A mappings file whose build_id is not a prefix of it is not used. NULL: not checked */
    const char *platform;        /* the library folder mods ship for this host: "windows-x64", "linux-x64", "macos-arm64". NULL: mt_platform() */

    /* One finished line (no newline) at a level MT_LOG_*. The core's own lines start "[modtopia] "; a mod's start "[<mod id>] ". NULL: stderr */
    void (*log)(void *user, int level, const char *line);

    /* Guest memory by guest address: copy n bytes. MT_OK, or MT_ERR_FAULT when any byte is outside the guest's own memory or not mapped, without
       crashing (guard the copy). Must never reach the host's own memory through a guest address. NULL: no running game */
    int (*mem_read)(void *user, uint64_t addr, void *out, size_t n);
    int (*mem_write)(void *user, uint64_t addr, const void *in, size_t n);
    /* A loaded module's guest base address: "main", "sdk", "subsdk0", "rtld". 0 when the game has no such module. NULL: no running game */
    uint64_t (*module_base)(void *user, const char *module);

    /* Registers at a hook stop. host_ctx is exactly what the host passed to mt_hook_dispatch for this stop; the values are the game's at that
       instruction. reg: 0..30 x0..x30, 31 sp, 32 pc (read only: MT_ERR_UNKNOWN on a write), 33 nzcv (N Z C V in bits 31..28), 34 fpcr, 35 fpsr.
       vreg: 0..31, 16 bytes each, little-endian as the guest stores them. A write takes effect when the stop returns. MT_OK or MT_ERR_UNKNOWN */
    int (*reg_get)(void *user, void *host_ctx, int reg, uint64_t *out);
    int (*reg_set)(void *user, void *host_ctx, int reg, uint64_t value);
    int (*vreg_get)(void *user, void *host_ctx, int vreg, void *out16);
    int (*vreg_set)(void *user, void *host_ctx, int vreg, const void *in16);

    /* Call the game function at addr on the calling thread, with x0..x7 and v0..v7 from regs, a fresh return address, and the thread's own stack.
       On return regs holds x0..x7 and v0..v7 as the function left them. The thread's registers afterwards are as before the call (only memory keeps
       what the function did). host_ctx is the stop the call is made from, or NULL (an event). MT_ERR_STATE when the thread is not a game thread */
    int (*call_guest)(void *user, void *host_ctx, uint64_t addr, MtCall *regs);
    /* For replace points: run the game's own function at site on host_ctx's registers (so with the arguments the stop has), skipping the stop at
       site this one time (a thread-local "skip site once" flag is enough), and leave its results in those registers. MT_OK */
    int (*call_original)(void *user, void *host_ctx, uint64_t site);

    /* 1 when the game has a stop of at least this kind at addr (observe 1 < take over 2 < replace 3), 0 when not. A mod whose points are not
       all built in is turned off with a message to rebuild. NULL: every point is (a host that patches stops in at run time) */
    int (*hook_built_in)(void *user, uint64_t addr, int kind);

    /* 1 when the game's own RomFS has this path ("motor_asset_re/x/y.fdata", relative, '/' separated), 0 when not. NULL: unknown (no data check) */
    int (*game_has_file)(void *user, const char *romfs_path);
    /* A note when replacing this game file needs more than a restart (e.g. "holds shaders, ...: use Refresh the graphics"), or NULL. May be NULL */
    const char *(*file_note)(void *user, const char *romfs_path);

    /* The game's own files, for patches (SPEC §10): the size of a file in its RomFS (path as for game_has_file, the overlay not counted), or -1
       when it has none; and n bytes of it from offset into buf: how many were read (n, or fewer only at its end), or -1 on an error. Called by
       mt_loader_apply_patches only, on its thread. NULL: this host cannot apply patches (mods that have patches/ get a note) */
    int64_t (*game_file_size)(void *user, const char *romfs_path);
    int64_t (*game_read)(void *user, const char *romfs_path, uint64_t offset, void *buf, uint64_t n);

    /* API 0.2: what mods use to reach the player. Each may be NULL: the mod's call then returns MT_ERR_VERSION (key_down 0, save_dir NULL).
       They are called on the calling mod's thread (a game thread, in a hook or an event) */
    /* A short message over the game's picture for seconds; a newer one from the same mod replaces its earlier one. MT_OK */
    int (*notify)(void *user, const char *mod_id, const char *text, double seconds);
    /* The next picture the game presents, as a PNG at path (absolute; its folder exists); the host's own menus left out. MT_OK when requested */
    int (*screenshot)(void *user, const char *path);
    /* The player's save folder (mods only copy from it) */
    const char *(*save_dir)(void *user);
    /* Player p's own input (p 0..7) as the game last read it: every device combined, before pad_block and pad_hold. MT_OK */
    int (*pad_read)(void *user, int p, MtPad *out);
    /* The input the mods add to player p's from now on (the core combines every mod's: buttons ORed, sticks added and clamped), or NULL for
       none: the host merges it into what the game reads until the next call. MT_OK */
    int (*pad_hold)(void *user, int p, const MtPad *add);
    /* 1 while the keyboard key is held: its name as key settings store it ("F2", "H", "Left Shift") */
    int (*key_down)(void *user, const char *key);
    /* The final picture's colour matrix (the core passes the last loaded mod's), or NULL for none: r' = m[0]r + m[1]g + m[2]b + m[3], ...
       in sRGB 0..1, applied after the game's own image and any upscaling. MT_OK */
    int (*color_matrix)(void *user, const float *m12);

    /* API 0.3 */
    /* The input of player p's that the game must not see from now on (the core combines every mod's): the buttons in mask, the left stick when
       its lx or ly is not 0, the right when rx or ry is not 0; NULL for none. Applied before pad_hold's input is added. MT_OK */
    int (*pad_block)(void *user, int p, const MtPad *mask);
    /* 0: leave out everything the game draws over its 3D picture (HUD, markers, menus, text); 1: draw it again (the core passes 0 while any
       mod hides it). MT_OK */
    int (*ui_visible)(void *user, int visible);

    /* API 0.4 */
    /* One HTTP(S) request, off the calling thread: method "GET" or "POST", url (the core has checked it against the mod's hosts), body (n
       bytes to send as application/json, NULL for none). done(token, status, body, n) is called once, from any thread: the HTTP status and
       the answer's body, or status 0 when there was no answer. MT_OK when it was started (then done will be called), anything else when
       not (then it must not be). NULL: no network for mods on this host (net_request returns MT_ERR_VERSION) */
    int (*net_request)(void *user, const char *method, const char *url, const char *body, size_t n,
                       void (*done)(void *token, int status, const char *body, size_t n), void *token);

    /* API 0.5: sound the mods add, mixed under the game's own output by the host. Each mod that plays has a voice (0 to
       MT_AUDIO_VOICES - 1, given out by the core). Any thread may call these. NULL: no sound for mods on this host */
    /* frames of PCM16 stereo at 48000 Hz for a voice: how many were taken (0: its buffer is full) */
    int (*audio_write)(void *user, int voice, const int16_t *stereo, int frames);
    int (*audio_queued)(void *user, int voice);   /* frames of the voice not yet played */
    int (*audio_clear)(void *user, int voice);    /* drop them. MT_OK */

    /* API 0.6: notify with a picture beside the text: MT_NOTIFY_IMAGE x MT_NOTIFY_IMAGE pixels of RGBA (not premultiplied; the core has
       read and fitted the mod's PNG; valid during the call). NULL: the core calls notify, and the text goes out alone */
    int (*notify_image)(void *user, const char *mod_id, const char *text, double seconds, const uint8_t *rgba);
} MtBackend;
#define MT_AUDIO_VOICES 8
#define MT_NOTIFY_IMAGE 128

typedef struct MtOptions {
    const char *mods_dir;        /* the game's mods folder (UTF-8). Required */
    const char *config_dir;      /* the players' settings, config/<id>.toml (NULL: <mods_dir>/../config) */
    const char *cache_dir;       /* unpacked packages and known-code.toml (NULL: <mods_dir>/.modtopia) */
    const char *mappings_dir;    /* the folder of <game version>.toml mappings (NULL: none: mapped names do not resolve, no AreaEntered) */
    int no_code;                 /* 1: no code is ever loaded (a launcher's plan, a safe mode); data mods still are */
} MtOptions;

/* ---------------------------------------------------------------- lifecycle */
/* A loader for one game. opt and be are copied (their strings must outlive the loader). One loader per process: the MtHost calls mods make
   without a mod (mem_read, call_guest, frame) reach the loader that was started last */
MtLoader *mt_loader_new(const MtOptions *opt, const MtBackend *be);
/* Frees the loader's lists. Unloads nothing: code mods stay mapped and their hooks stay registered until the process ends */
void mt_loader_free(MtLoader *L);
/* Reads mods/modtopia.toml and the mappings, finds every mod in mods_dir (.pkmod / .zip packages, mod folders, LayeredFS folders and zips),
   unpacks packages (once per checksum, into cache_dir; stale unpacked copies are removed), reads each manifest, resolves versions, dependencies,
   order and conflicts, loads each mod's settings, builds the hook plan and the overlay list, and (with module_base) the hook sites.
   Logs nothing for a healthy mod; each mod's state and message say what happened. 0, or -1 when mods_dir is not a folder (then nothing loads) */
int mt_loader_scan(MtLoader *L);
/* For each mod that is on, in load order: checks its points (the word at each address with mem_read; hook_built_in), then loads its library,
   checks its SDK version (same major, minor not above the core's) and calls its entry point with the MtHost table. A mod that fails any step
   is off or in error with a message; the others carry on. Call it before the game runs, after the overlay is applied. Returns how many code
   mods started */
int mt_loader_start(MtLoader *L);
/* Once a game frame (at each present), on the game's thread: posts FrameStart, and every 8 frames reads the mappings' [core] area: AreaEntered
   when the area changes while the world is up, SaveLoaded each time the world comes up from the title (the ready field turns non-zero) */
void mt_loader_frame(MtLoader *L);
/* After the game commits its save: posts SaveWritten */
void mt_loader_save_written(MtLoader *L);
/* This build's platform tag: "windows-x64", "windows-arm64", "linux-x64", "linux-arm64", "macos-arm64", "android-arm64" */
const char *mt_platform(void);

/* ---------------------------------------------------------------- hooks */
/* The game reached the built-in point at site (its guest address). Runs the handlers registered there, in load order, each with a context whose
   registers are host_ctx's. MT_HANDLED when a take-over or replace handler said so (the host then returns from the game function at once with the
   registers as they are); MT_CONTINUE otherwise, including at an address no mod uses. No lock; safe on many threads */
int mt_hook_dispatch(MtLoader *L, uint64_t site, void *host_ctx);
/* 1 when a handler is registered at site */
int mt_hook_active(MtLoader *L, uint64_t site);

/* The hook plan: every point the mods that are on declare for this game version, one entry per address (the strongest kind declared there:
   replace > take_over > observe), sorted by module then offset. The host builds a stop at each before running the game; the plan's hash goes
   with the build, so a launcher knows when to rebuild */
typedef struct MtPlanPoint {
    char module[16];    /* "main", "sdk", ... */
    uint64_t offset;    /* from the module's base */
    uint32_t word;      /* the instruction expected there: refuse to build the point when it differs */
    int kind;           /* MT_HOOK_*: replace is at a function's entry and stops once a call; the others stop before the instruction each time */
} MtPlanPoint;
int mt_plan_count(MtLoader *L);
const MtPlanPoint *mt_plan_point(MtLoader *L, int i);                /* NULL out of range */
const char *mt_plan_hash(MtLoader *L);                               /* SHA-256 hex of the canonical plan lines; "" when there are no points */
/* The hooks file: "# modtopia hooks 1", "# game <v>", "# hash <h>", a header comment, then "module<TAB>offset<TAB>kind<TAB>word" a point
   (offset and word in 0x hex, kind observe / take_over / replace). 0, or -1 when it cannot be written */
int mt_plan_write(MtLoader *L, const char *path);

/* ---------------------------------------------------------------- the data overlay (LayeredFS) */
/* The folders to lay over the game's RomFS, absolute: index 0 has the highest priority (the last mod loaded wins on the same path); folders
   merge, files only a mod has are added. Apply them before the game mounts its RomFS */
int mt_loader_overlay_count(MtLoader *L);
const char *mt_loader_overlay_root(MtLoader *L, int i);
const char *mt_loader_overlay_mod(MtLoader *L, int i);               /* the id of the mod root i belongs to */
/* Counts each data mod's files against the game's own (game_has_file) and logs a line per mod. A mod none of whose files is one of the game's
   gets a warning and a note (usually the wrong folder level); a file with a file_note gets that note. Call it once the game's files can be read */
void mt_loader_check_data(MtLoader *L);
/* Applies the patches/ of every mod that is on, in load order, to the game's own files (game_file_size, game_read): each patch file
   patches/<RomFS path>.mtpatch checks the game file (size and sha256), then writes the patched file into cache_dir/patched/<id>-<key>/files/,
   once per patch and game build (afterwards it is reused). That folder joins the overlay list right after the mod's own data folder (its own
   files win over its patched ones). A patch whose added bytes include the game's own data is refused: the whole mod is turned off now and at
   every later start. A patch for another game version, or for a file the game lacks, is left out with a warning and a note. Returns how many
   game files are patched; when more than 0, lay the overlay again (mt_loader_overlay_count / _root). Call it once, when the game's files can be
   read and before the game reads them. A file of gigabytes is streamed; its first patching reads it twice and writes a full copy */
int mt_loader_apply_patches(MtLoader *L);

/* The hook points of the mods that are on, with their guest addresses (0 when module_base had no such module), for a host that runs some game
   functions another way (an interpreter) and must tell which points that skips. mt_loader_point: MT_OK, or MT_ERR_UNKNOWN out of range */
int mt_loader_point_count(MtLoader *L);
int mt_loader_point(MtLoader *L, int i, uint64_t *addr, int *kind, const char **mod_id, const char **name);
/* Adds a note to a mod's message (shown with it in a mod list and in MtModInfo.message), e.g. that a host feature skips one of its points */
void mt_loader_note(MtLoader *L, const char *id, const char *text);

/* ---------------------------------------------------------------- the mods, for a mod list (F1, a launcher, the CLI) */
enum { MT_STATE_OK = 0, MT_STATE_OFF = 1, MT_STATE_ERROR = 2 };
enum { MT_MOD_HAS_CODE = 1, MT_MOD_HAS_DATA = 2, MT_MOD_LAYEREDFS = 4, MT_MOD_GAMEPLAY = 8, MT_MOD_NEW_CODE = 16,
       MT_MOD_KIND_GUESSED = 32,     /* GAMEPLAY decided by the loader from a LayeredFS mod's files (see mt_loader_set_affects) */
       MT_MOD_HAS_PATCHES = 64,      /* patches/ against the game's own files */
       MT_MOD_NETWORK = 128 };       /* its mod.toml names hosts it may reach (MtModInfo.network) */
typedef struct MtModInfo {
    const char *id, *name, *version, *authors, *description, *license;
    const char *source;      /* the file or folder in mods/ */
    const char *sha256;      /* the package's checksum (a folder: of its mod.toml and its library) */
    int flags;               /* MT_MOD_* */
    int state;               /* MT_STATE_OK: loads (or loaded); OFF: not this time (switched off, another game version, ...); ERROR: broken */
    int enabled;             /* the player's switch (takes effect at the next start) */
    int started;             /* its code ran (mt_loader_start) */
    const char *message;     /* why it is off or broken, or a warning or note; "" when there is nothing to say */
    int load_index;          /* its place in the load order, -1 when it does not load */
    int hook_points;         /* points declared for this game version */
    int patches;             /* its patch files (patches/), 0 when none */
    const char *network;     /* the hosts it may reach, joined with ", "; "" when none */
} MtModInfo;
/* Every mod found: those that load, in load order, then the rest. Indexes are valid until the next scan */
int mt_loader_count(MtLoader *L);
const MtModInfo *mt_loader_info(MtLoader *L, int i);                 /* NULL out of range */
int mt_loader_find(MtLoader *L, const char *id);                     /* its index, -1 when none */

/* The player's choices, saved at once to mods/modtopia.toml (0, or -1 when it cannot be written); they take effect at the next start */
int mt_loader_set_enabled(MtLoader *L, const char *id, int on);
int mt_loader_move(MtLoader *L, const char *id, int delta);           /* earlier (-1) or later (+1) in the player's order; MT_ERR_STATE at an end */
int mt_loader_save_list(MtLoader *L);
/* A LayeredFS mod (no mod.toml) counts as gameplay for Link Play unless every one of its files is a visual-only kind (textures, movies, fonts,
   sound, UI layouts): MT_MOD_KIND_GUESSED then marks the guess. The player can say otherwise, "gameplay" or "visual", or NULL for the loader's
   guess again ([affects] in mods/modtopia.toml). MT_ERR_UNKNOWN for any other word (the choice already saved stays) */
int mt_loader_set_affects(MtLoader *L, const char *id, const char *affects);
/* Code mods are pointed out before they first run (MT_MOD_NEW_CODE: new, or changed since the player last allowed it). Once the player agrees,
   this records the checksum of every code mod found now (known-code.toml in cache_dir) */
int mt_loader_trust_code(MtLoader *L);
/* The same for the one mod of that id: what an installer records after the player's yes for the file it just put in place, so other
   new code is still pointed out. MT_ERR_UNKNOWN when there is no mod of that id with code */
int mt_loader_trust_mod(MtLoader *L, const char *id);

/* ---------------------------------------------------------------- settings, for a settings screen (mod: the mod list's index) */
enum { MT_SET_BOOL = 1, MT_SET_NUMBER = 2, MT_SET_CHOICE = 3, MT_SET_TEXT = 4, MT_SET_KEY = 5 };
typedef struct MtSettingInfo {
    const char *key, *label, *description;
    int type, restart;          /* restart: a change takes effect at the next start (the mod is not told now) */
    double min, max, step;      /* numbers */
    int nchoices;
    const char *const *choices; /* choices */
} MtSettingInfo;
int mt_setting_count(MtLoader *L, int mod);
const MtSettingInfo *mt_setting_info(MtLoader *L, int mod, int i);
int mt_setting_get_text(MtLoader *L, int mod, int i, char *out, size_t n);   /* the value as shown: On / Off, a number, the choice, the text */
double mt_setting_get_number(MtLoader *L, int mod, int i);                   /* bool 0/1, number, choice index */
/* A new value: clamped to min..max and the step (numbers), wrapped into range (choices), 0/1 (bools); saved to config/<id>.toml; then the mod's
   on_config_changed callback runs on the calling thread unless the setting is restart. MT_OK, MT_ERR_UNKNOWN (no such setting), MT_ERR_STATE
   (text through set_number, or the reverse) */
int mt_setting_set_number(MtLoader *L, int mod, int i, double v);
int mt_setting_set_text(MtLoader *L, int mod, int i, const char *v);         /* text, key, or a choice by its name */

/* Link Play (SPEC §12): calls fn with "id@version" of every gameplay mod that is on (code mods only when they started), in load order; returns
   how many. A host sorts them and adds each to the session's mods fingerprint after its own features, so two hosts refuse the same mismatches */
int mt_loader_gameplay_mods(MtLoader *L, void (*fn)(void *user, const char *id_at_version), void *user);

/* ---------------------------------------------------------------- utilities */
int mt_version_satisfies(const char *version, const char *range);   /* 1 yes, 0 no, -1 a malformed version or range */
int mt_version_compare(const char *a, const char *b);               /* <0, 0, >0 by semantic version precedence */

#ifdef __cplusplus
}
#endif
#endif
