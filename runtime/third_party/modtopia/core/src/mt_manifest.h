/* Modtopia core: mod.toml as the core reads it (docs/mod-toml.md). MIT. */
#ifndef MT_MANIFEST_H
#define MT_MANIFEST_H

#include "mt_internal.h"

typedef struct MtDep {
    char *id, *range;
    int required;
    int order;   /* -1 before the dependency, +1 after it (the default), 0 none */
} MtDep;

typedef struct MtSetting {
    MtSettingInfo info;   /* key, label, ... (strings owned here) */
    double def_num;       /* bool, number, choice index */
    char *def_text;       /* text and key */
    MtStrs choices;
} MtSetting;

typedef struct MtHookDecl {
    char *name;
    char *target;   /* "main+0x1234" or a mapped name */
    int64_t offset; /* added to a mapped name's address (0: its entry) */
    int64_t word;   /* the expected instruction word; -1 none given */
    int kind;       /* MT_HOOK_* */
} MtHookDecl;

typedef struct MtHookSet {
    char *game;
    MtHookDecl *h;
    int n;
} MtHookSet;

typedef struct MtManifest {
    char *id, *name, *version, *description, *license, *homepage, *icon, *loader, *entry, *data;
    char *authors;   /* joined with ", " */
    MtStrs games, platforms;
    MtStrs network;  /* the hosts it may reach (API 0.4) */
    int gameplay;    /* affects = "gameplay" */
    MtDep *deps;
    int ndeps;
    MtSetting *settings;
    int nsettings;
    MtHookSet *hooks;
    int nhooks;
} MtManifest;

/* 0, or -1 with err set (a mod that cannot be used, with a message for the player and the author) */
int mt_manifest_parse(const char *text, MtManifest *m, char *err, size_t errn);
void mt_manifest_free(MtManifest *m);
int mt_id_valid(const char *id);
const MtHookSet *mt_manifest_hooks_for(const MtManifest *m, const char *game);

#endif
