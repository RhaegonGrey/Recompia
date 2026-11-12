/* Modtopia core: the loader's state, shared by mt_loader.c (finding and resolving mods) and mt_runtime.c (running them). MIT. */
#ifndef MT_LOADER_H
#define MT_LOADER_H

#include "mt_manifest.h"

#include <stdatomic.h>

#define MT_PLAYERS 8

typedef struct MtPoint {
    char *name;
    char module[16];
    uint64_t offset;
    uint32_t word;
    int kind;
    uint64_t addr;   /* in the running game (mt_loader_start) */
} MtPoint;

typedef struct MtCfgVal { double num; char *text; } MtCfgVal;

struct MtMod {
    MtLoader *L;
    MtModInfo info;      /* what a mod list shows; its strings point into the fields below */
    MtManifest m;
    int has_manifest;
    char *source, *dir, *data_root, *lib_path, *sha, *msg, *layered_name;
    char *patch_root;    /* its patches/ folder, when it has .mtpatch files */
    char *patched_root;  /* the folder of its patched game files (mt_loader_apply_patches) */
    int npatches;
    char *data_dir;      /* config/<id>/, made when the mod first asks */
    char *net_text;      /* its hosts joined, for MtModInfo.network */
    MtPad hold[MT_PLAYERS];
    uint8_t holding[MT_PLAYERS];
    float cmat[12];
    int has_cmat;
    MtPad block[MT_PLAYERS];
    uint8_t blocking[MT_PLAYERS];
    int hides_ui;
    int voice;           /* its sound voice + 1 (API 0.5), 0 while it has not played */
    MtPoint *pts;
    int npts;
    MtCfgVal *cfg;
    MtConfigFn cfg_fn;
    void *cfg_user;
    void *lib;
    int order_pos;       /* the player's order (mods/modtopia.toml), INT_MAX when not listed */
};
typedef struct MtMod Mod;
int mt_net_request(MtMod *m, const char *method, const char *url, const char *body, MtNetFn done, void *user);

typedef struct HookReg { Mod *mod; int kind; MtHookFn fn; void *user; } HookReg;
typedef struct Site {
    uint64_t addr;
    int cap;
    _Atomic int n;
    HookReg *h;
    int said;   /* who took it was logged */
} Site;

typedef struct Sub { Mod *mod; int prio, seq; MtEventFn fn; void *user; } Sub;
typedef struct SubList { int n; Sub s[]; } SubList;
typedef struct EventSlot { char *name; SubList *volatile list; } EventSlot;

struct MtLoader {
    MtBackend be;
    char *mods_dir, *config_dir, *cache_dir, *mappings_dir;
    int no_code;
    Mod **mods;
    int nmods;
    Mod **order;
    int norder;
    MtStrs player_order, disabled, affects_ids, affects_vals;   /* mods/modtopia.toml: order, disabled, [affects] */
    MtStrs known_ids, known_shas;   /* code the player has already allowed (.modtopia/known-code.toml) */
    MtPlanPoint *plan;
    int nplan;
    char plan_hash[65];
    MtStrs overlay, overlay_ids;
    MtStrs refused;   /* sha256 of patches refused for carrying the game's own data (.modtopia/patched/refused.txt) */
    MtToml *mappings;
    MtHost host;
    MtLock lock;
    int voices;          /* sound voices given out */
    Site *sites;
    int site_cap;
    EventSlot *events;
    int nevents, events_cap, sub_seq;
    volatile uint64_t frame;
    int32_t area;
    int area_valid;
    uint32_t saves_loaded, saves_written;
    uint64_t area_global;
    uint32_t area_off, area_size;
    uint32_t ready_off, ready_size;   /* [core] area's ready field: the world is up while it is not zero (0 size: no such field) */
    int world;                        /* the world was up at the last read */
    int started;
};

/* mt_loader.c */
int mt_mod_is_ok(const Mod *m);
const char *mt_mapping_area_name(MtLoader *L, int32_t area);
uint64_t mt_resolve_name(MtLoader *L, const char *name, uint32_t *word, char module[16], uint64_t *offset);
void mt_mod_message(Mod *m, int state, const char *fmt, ...);
void mt_info_refresh(Mod *m);

/* mt_runtime.c */
void mt_host_init(MtLoader *L);
void mt_sites_build(MtLoader *L);
void mt_config_notify(Mod *m, const char *key);

#endif
