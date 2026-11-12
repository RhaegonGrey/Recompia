/* Modtopia SDK: what a mod sees. A stable C ABI (MIT), the same on every host that implements Modtopia.
 *
 * A code mod is a native library (mod.dll, mod.so, ...) that exports its entry point (mod.toml `entry`, default "mod_init"):
 *
 *     #include <modtopia/modtopia.h>
 *     MT_DECLARE_MOD()
 *     MT_EXPORT int mod_init(const MtHost *host, MtMod *mod) { ...; return MT_OK; }
 *
 * The host passes its function table and the mod's handle; the mod registers its hooks, event handlers and config callbacks and returns.
 * Everything a mod touches is about the game, never a host's internals:
 *   - game functions and instructions are named in mod.toml (a mapped name, or module + offset with the expected instruction word);
 *     code refers to them by the hook point's `name`, so the same code runs on every game version the manifest covers;
 *   - game memory is reached by guest address through mem_read / mem_write (never a raw pointer: a guest address is not the same native
 *     address on every host);
 *   - registers are reached through reg_get / reg_set (no fixed struct: hosts lay their register files out differently);
 *   - game files are named by their RomFS path.
 *
 * Versioning: MT_API_VERSION = major << 16 | minor. A host loads a mod built against the same major and a minor up to its own. Fields are only
 * ever added at the end of MtHost; a mod checks host->size before using a field newer than the minor it was built for (MT_HOST_HAS).
 *
 * Threads: hooks run on the game's own threads, possibly several at once. Event handlers run on the thread that posts the event (the core
 * events: a game thread). Keep handlers short; never block in them. */
#ifndef MODTOPIA_H
#define MODTOPIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_API_VERSION_MAJOR 0
#define MT_API_VERSION_MINOR 6
#define MT_API_VERSION ((uint32_t)(MT_API_VERSION_MAJOR << 16 | MT_API_VERSION_MINOR))

#if defined(_WIN32)
#define MT_EXPORT __declspec(dllexport)
#else
#define MT_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define MT_EXTERN_C extern "C"
#else
#define MT_EXTERN_C
#endif

/* Every mod library declares the SDK version it was built against with this, once. C++ mods write their entry point as
   MT_EXTERN_C MT_EXPORT int mod_init(const MtHost *host, MtMod *mod) */
#define MT_DECLARE_MOD() MT_EXTERN_C MT_EXPORT const uint32_t modtopia_api_version = MT_API_VERSION;

typedef struct MtHost MtHost;       /* the host's function table */
typedef struct MtMod MtMod;         /* this mod (opaque) */
typedef struct MtHookCtx MtHookCtx; /* one stop at a hook point (opaque, valid only during the handler) */

/* Results */
enum {
    MT_OK = 0,
    MT_ERR = -1,          /* a generic failure (the log says why) */
    MT_ERR_UNKNOWN = -2,  /* no such hook point, setting, event or name */
    MT_ERR_FAULT = -3,    /* a guest address that is not mapped */
    MT_ERR_STATE = -4,    /* not allowed here (a register write in an observe hook, a guest call off a game thread, ...) */
    MT_ERR_VERSION = -5   /* the host is older than this call */
};

/* Log levels */
enum { MT_LOG_DEBUG = 0, MT_LOG_INFO = 1, MT_LOG_WARN = 2, MT_LOG_ERROR = 3 };

/* ---------------------------------------------------------------- hooks (the Mixin layer, SPEC §8)
 * The kind is declared per hook point in mod.toml; the handler's return value means:
 *   observe   : MT_CONTINUE only (the game carries on unchanged; register writes are refused)
 *   take_over : MT_CONTINUE (the game carries on with the registers and memory as the handler left them) or MT_HANDLED (the function returns at
 *               once: leave the registers as a normal return would, x0/v0 the result, sp and the callee-saved registers as at its entry)
 *   replace   : at the function's entry; MT_HANDLED (the mod's function ran instead) or MT_CONTINUE (the game's own function runs after all).
 *               call_original runs the game's function from inside the handler.
 * Several mods may share a point: they run in load order and the first MT_HANDLED wins (the host logs who took it). */
enum { MT_CONTINUE = 0, MT_HANDLED = 1 };
enum { MT_HOOK_OBSERVE = 1, MT_HOOK_TAKE_OVER = 2, MT_HOOK_REPLACE = 3 };
typedef int (*MtHookFn)(MtHookCtx *ctx, void *user);

/* Registers for reg_get / reg_set: x0..x30 are 0..30 */
enum { MT_REG_X0 = 0, MT_REG_X30 = 30, MT_REG_SP = 31, MT_REG_PC = 32, MT_REG_NZCV = 33, MT_REG_FPCR = 34, MT_REG_FPSR = 35 };

/* ---------------------------------------------------------------- events (SPEC §7)
 * Core events (names): "FrameStart", "SaveLoaded", "SaveWritten", "AreaEntered". Mods may post their own, named "<mod id>:<name>".
 * Priorities run highest first; a cancellable event stops at the first handler that returns MT_CANCEL. */
enum { MT_PRIORITY_HIGHEST = 200, MT_PRIORITY_HIGH = 150, MT_PRIORITY_NORMAL = 100, MT_PRIORITY_LOW = 50, MT_PRIORITY_LOWEST = 0 };
enum { MT_EVENT_CONTINUE = 0, MT_CANCEL = 1 };

typedef struct MtEvent {
    const char *name;
    const void *data;     /* the event's data (below for core events), or what the poster passed */
    uint32_t size;        /* its size in bytes */
    uint32_t cancellable; /* 1: MT_CANCEL stops it */
    uint64_t frame;       /* the game frame it was posted in (FrameStart's count) */
} MtEvent;
typedef int (*MtEventFn)(MtMod *mod, const MtEvent *ev, void *user);

typedef struct MtFrameData { uint64_t frame; } MtFrameData;                               /* FrameStart */
typedef struct MtAreaData { int32_t area; int32_t previous; const char *name; } MtAreaData; /* AreaEntered: the game's area (stage) id; name or NULL */
typedef struct MtSaveData { uint32_t count; } MtSaveData;                                  /* SaveLoaded / SaveWritten: how many so far this run */

/* ---------------------------------------------------------------- guest calls
 * x0..x7 and v0..v7 in; on return x0..x7 and v0..v7 hold what the game function left (x0/x1 and v0..v3 are its results). Only on a game
 * thread: inside a hook, or in an event the game's thread posts. */
typedef struct MtCall {
    uint64_t x[8];
    uint8_t v[8][16];
} MtCall;

/* ---------------------------------------------------------------- the network (API 0.4)
 * A mod reaches only the hosts its mod.toml names (network = ["stats.example.org"]); mod lists show them to the player. The answer to a
 * request: the HTTP status (0: there was none) and the body (n bytes, ending in a 0, valid during the call), on the game's thread. */
typedef void (*MtNetFn)(MtMod *mod, int status, const char *body, size_t n, void *user);

/* ---------------------------------------------------------------- the player's input (API 0.2)
 * A Switch controller's state: buttons as bits (MT_PAD_*), sticks from -32767 to 32767 (up and right are positive) */
enum {
    MT_PAD_A = 1u << 0, MT_PAD_B = 1u << 1, MT_PAD_X = 1u << 2, MT_PAD_Y = 1u << 3, MT_PAD_LSTICK = 1u << 4, MT_PAD_RSTICK = 1u << 5,
    MT_PAD_L = 1u << 6, MT_PAD_R = 1u << 7, MT_PAD_ZL = 1u << 8, MT_PAD_ZR = 1u << 9, MT_PAD_PLUS = 1u << 10, MT_PAD_MINUS = 1u << 11,
    MT_PAD_LEFT = 1u << 12, MT_PAD_UP = 1u << 13, MT_PAD_RIGHT = 1u << 14, MT_PAD_DOWN = 1u << 15
};
typedef struct MtPad {
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
} MtPad;

/* ---------------------------------------------------------------- config (SPEC §11)
 * The settings are declared in mod.toml ([[settings]]); the player's values live in config/<id>.toml. A callback hears every change the
 * player makes while the game runs (settings marked restart = true change at the next start instead). */
typedef void (*MtConfigFn)(MtMod *mod, const char *key, void *user);

/* ---------------------------------------------------------------- the host's table */
struct MtHost {
    uint32_t size;              /* sizeof(MtHost) as the host knows it */
    uint32_t api_version;       /* the host's MT_API_VERSION */
    const char *host_name;      /* e.g. "Pokopia Native 1.5.0" (for logs; never branch on it) */
    const char *game_version;   /* "2.0.1", "1.0.5" */
    const char *game_build_id;  /* main's build ID, hex */
    const char *platform;       /* "windows-x64", "linux-x64", "macos-arm64", ... */

    /* the log (one line; the host adds the mod's id) */
    void (*log)(MtMod *mod, int level, const char *line);

    /* this mod */
    const char *(*mod_id)(MtMod *mod);
    const char *(*mod_version)(MtMod *mod);
    const char *(*mod_dir)(MtMod *mod);   /* its unpacked package folder (UTF-8), for files of its own */

    /* hooks: a hook point by its name in mod.toml, for the game version being played */
    int (*hook)(MtMod *mod, const char *point, MtHookFn fn, void *user);
    uint64_t (*reg_get)(MtHookCtx *ctx, int reg);
    int (*reg_set)(MtHookCtx *ctx, int reg, uint64_t value);
    int (*vreg_get)(MtHookCtx *ctx, int vreg, void *out16);        /* v0..v31, 16 bytes */
    int (*vreg_set)(MtHookCtx *ctx, int vreg, const void *in16);
    uint64_t (*hook_address)(MtHookCtx *ctx);                       /* the point's guest address */
    int (*hook_kind)(MtHookCtx *ctx);                               /* MT_HOOK_* */
    int (*call_original)(MtHookCtx *ctx);                           /* replace: the game's function with the current registers */

    /* game memory, by guest address */
    int (*mem_read)(uint64_t addr, void *out, size_t n);
    int (*mem_write)(uint64_t addr, const void *in, size_t n);
    uint64_t (*module_base)(const char *module);                    /* "main", "sdk", "subsdk0"; 0 if none */
    uint64_t (*resolve)(MtMod *mod, const char *name);              /* a mapped name or "main+0x1234": its guest address now, 0 if unknown */

    /* calling the game */
    int (*call_guest)(uint64_t addr, MtCall *regs);

    /* events */
    int (*subscribe)(MtMod *mod, const char *event, int priority, MtEventFn fn, void *user);
    int (*post)(MtMod *mod, const char *event, const void *data, uint32_t size, int cancellable);   /* 1: a handler cancelled it */

    /* config */
    int (*config_bool)(MtMod *mod, const char *key);
    double (*config_number)(MtMod *mod, const char *key);
    const char *(*config_string)(MtMod *mod, const char *key);      /* choice, text and key settings; valid until the next change */
    int (*on_config_changed)(MtMod *mod, MtConfigFn fn, void *user);

    uint64_t (*frame)(void);                                         /* the current game frame (FrameStart's count) */

    /* ---- API 0.2: talking to the player, the player's input, the picture. Check MT_HOST_HAS(host, field) before a call: an older host lacks
       these fields, and a host may set one to NULL when it cannot do it (then the mod does without) */
    /* A short message over the game's picture for that many seconds (e.g. "Photo saved"). A newer message replaces this mod's earlier one */
    int (*notify)(MtMod *mod, const char *text, double seconds);
    /* Saves the next picture the game shows as a PNG. A relative path is under the game's screenshots folder; the full path is written to out
       (out_n bytes) when out is not NULL. The host's own menus are not in it */
    int (*screenshot)(MtMod *mod, const char *path, char *out, size_t out_n);
    /* The player's save folder (only to copy from: never write in it), and a folder this mod may keep its own files in (made on first use) */
    const char *(*save_dir)(MtMod *mod);
    const char *(*data_dir)(MtMod *mod);
    /* Player p's own input (0 is the first player) as the game last asked for it: every device combined, before what mods hold or block */
    int (*pad_read)(MtMod *mod, int p, MtPad *out);
    /* Input this mod adds to player p's until its next call: buttons held, and stick values added (clamped). NULL lets go */
    int (*pad_hold)(MtMod *mod, int p, const MtPad *add);
    /* 1 while a keyboard key is held, by its name as a key setting stores it ("F2", "H", "Left Shift"), 0 when not or unknown */
    int (*key_down)(MtMod *mod, const char *key);
    /* A colour matrix over the final picture, after the game's own image: in sRGB, r' = m[0] r + m[1] g + m[2] b + m[3], g' = m[4..7],
       b' = m[8..11], channels 0..1. NULL takes this mod's matrix away. When several mods set one, the last mod loaded wins */
    int (*color_matrix)(MtMod *mod, const float *m12);

    /* ---- API 0.3 */
    /* Input of player p's the game no longer sees, until this mod's next call: the buttons set in mask, and a stick whose value in mask is
       not 0 (lx or ly: the left stick, rx or ry: the right). pad_read still returns it, and what mods hold is added after. NULL: nothing */
    int (*pad_block)(MtMod *mod, int p, const MtPad *mask);
    /* 0 hides everything the game draws over its 3D picture (HUD, markers, menus, text), 1 shows it again. It stays hidden while any mod
       hides it. The host's own menu is not part of it */
    int (*ui_visible)(MtMod *mod, int visible);

    /* ---- API 0.4 */
    /* One request to a host this mod's mod.toml names: method "GET" or "POST", url "https://<host>/...", body a text to send (JSON) or
       NULL, at most 64 KiB. It returns at once; done runs at the start of a later frame. An answer over 256 KiB counts as none. At most 4
       of a mod's requests wait at a time. "http://" is taken for localhost and 127.0.0.1 only (a mod's own tests). MT_ERR_STATE: the
       address is not one the mod.toml names, or too many wait */
    int (*net_request)(MtMod *mod, const char *method, const char *url, const char *body, MtNetFn done, void *user);

    /* ---- API 0.5: sound. These three may also be called from a thread of the mod's own */
    /* Adds sound to what the player hears, under the game's own: frames of 16-bit stereo at 48000 Hz (left, right, left, ...). Returns
       how many frames were taken, 0 when the host's buffer for this mod is full (it keeps about a second: write the rest later), or a
       negative error. Nothing is heard while the game has no sound output open */
    int (*audio_write)(MtMod *mod, const int16_t *stereo, int frames);
    /* How many of this mod's frames are still waiting to be played */
    int (*audio_queued)(MtMod *mod);
    /* Drops this mod's waiting frames (a track stopped or was skipped) */
    int (*audio_clear)(MtMod *mod);

    /* ---- API 0.6 */
    /* notify with a small picture beside the text: a PNG file's path (8 bits a channel, not interlaced; best square, 128 pixels or more
       a side). Where the host cannot show pictures, or the file cannot be read, the text is shown alone. A mod's own files are under
       mod_dir */
    int (*notify_image)(MtMod *mod, const char *text, double seconds, const char *png_path);
};

#define MT_HOST_HAS(host, field) ((host)->size >= offsetof(MtHost, field) + sizeof((host)->field))

/* The entry point a code mod exports (mod.toml `entry`, default "mod_init") */
typedef int (*MtModInitFn)(const MtHost *host, MtMod *mod);

/* ---------------------------------------------------------------- helpers (inline: no host change needed) */
#include <stdarg.h>
#include <stdio.h>
static inline void mt_logf(const MtHost *h, MtMod *m, int level, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    h->log(m, level, line);
}
static inline float mt_vreg_f32(const MtHost *h, MtHookCtx *c, int v) {
    uint8_t b[16];
    float f = 0;
    if (h->vreg_get(c, v, b) == MT_OK) { for (size_t i = 0; i < sizeof f; i++) ((uint8_t *)&f)[i] = b[i]; }
    return f;
}
static inline int mt_vreg_set_f32(const MtHost *h, MtHookCtx *c, int v, float f) {   /* the low 32 bits; the rest cleared as a scalar write does */
    uint8_t b[16] = {0};
    for (size_t i = 0; i < sizeof f; i++) b[i] = ((const uint8_t *)&f)[i];
    return h->vreg_set(c, v, b);
}
static inline uint32_t mt_read_u32(const MtHost *h, uint64_t addr, int *ok) {
    uint32_t v = 0;
    int r = h->mem_read(addr, &v, sizeof v);
    if (ok) *ok = r == MT_OK;
    return v;
}

#ifdef __cplusplus
}
#endif
#endif
