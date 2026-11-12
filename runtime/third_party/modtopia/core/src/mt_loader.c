/* Modtopia core: finding the mods (packages, folders, LayeredFS data), unpacking, resolving versions, dependencies and order, the player's
   switches and settings, the hook plan and the data overlay. MIT. */
#include "mt_loader.h"
#include "mt_patch.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

void mt_log(MtLoader *L, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = mt_vfmt(fmt, ap);
    va_end(ap);
    char *line = mt_fmt("[modtopia] %s", s);
    if (L && L->be.log) L->be.log(L->be.user, level, line);
    else fprintf(stderr, "%s\n", line);
    free(line);
    free(s);
}

const char *mt_platform(void) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(_WIN32)
    return "windows-arm64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__ANDROID__)
    return "android-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#else
    return "unknown";
#endif
}
static const char *lib_name(const char *platform) {
    if (!strncmp(platform, "windows", 7)) return "mod.dll";
    if (!strncmp(platform, "macos", 5)) return "mod.dylib";
    return "mod.so";
}

static Mod *listed(MtLoader *L, int i);
int mt_mod_is_ok(const Mod *m) { return m->info.state == MT_STATE_OK; }
void mt_mod_message(Mod *m, int state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = mt_vfmt(fmt, ap);
    va_end(ap);
    free(m->msg);
    m->msg = s;
    if (state >= 0) m->info.state = state;
    m->info.message = m->msg;
}
void mt_info_refresh(Mod *m) {
    MtModInfo *i = &m->info;
    i->id = m->m.id;
    i->name = m->m.name;
    i->version = m->m.version;
    i->authors = m->m.authors;
    i->description = m->m.description;
    i->license = m->m.license;
    i->source = m->source;
    i->sha256 = m->sha ? m->sha : "";
    i->message = m->msg ? m->msg : "";
    i->hook_points = m->npts;
    i->patches = m->npatches;
    if (!m->net_text) {
        MtBuf b = {0};
        for (int k = 0; k < m->m.network.n; k++) mt_buf_fmt(&b, "%s%s", k ? ", " : "", m->m.network.v[k]);
        m->net_text = m->m.network.n ? mt_buf_take(&b) : mt_strdup("");
    }
    i->network = m->net_text;
    if (m->m.network.n) i->flags |= MT_MOD_NETWORK;
}

/* ---------------------------------------------------------------- the loader */
MtLoader *mt_loader_new(const MtOptions *opt, const MtBackend *be) {
    MtLoader *L = mt_alloc(sizeof *L);
    if (be) memcpy(&L->be, be, be->size < sizeof L->be ? be->size : sizeof L->be);
    L->be.size = sizeof L->be;
    if (!L->be.platform) L->be.platform = mt_platform();
    if (!L->be.host_name) L->be.host_name = "a Modtopia host";
    L->mods_dir = mt_path_abs(opt->mods_dir);
    char *parent = mt_path_join(L->mods_dir, "..");
    char *cfg = mt_path_join(parent, "config");
    L->config_dir = opt->config_dir ? mt_path_abs(opt->config_dir) : mt_path_abs(cfg);
    free(cfg);
    free(parent);
    char *cache = mt_path_join(L->mods_dir, ".modtopia");
    L->cache_dir = opt->cache_dir ? mt_path_abs(opt->cache_dir) : cache;
    if (opt->cache_dir) free(cache);
    L->mappings_dir = opt->mappings_dir ? mt_path_abs(opt->mappings_dir) : NULL;
    L->no_code = opt->no_code;
    L->area = -1;
    L->world = 0;
    mt_lock_init(&L->lock);
    mt_net_init();
    mt_host_init(L);
    return L;
}

static void mod_free(Mod *m) {
    mt_manifest_free(&m->m);
    free(m->source), free(m->dir), free(m->data_root), free(m->lib_path), free(m->sha), free(m->msg), free(m->layered_name);
    free(m->patch_root), free(m->patched_root), free(m->data_dir), free(m->net_text);
    for (int i = 0; i < m->npts; i++) free(m->pts[i].name);
    free(m->pts);
    if (m->cfg) {
        for (int i = 0; i < m->m.nsettings; i++) free(m->cfg[i].text);
        free(m->cfg);
    }
    free(m);
}
void mt_loader_free(MtLoader *L) {
    if (!L) return;
    mt_net_forget(L);
    for (int i = 0; i < L->nmods; i++) mod_free(L->mods[i]);
    free(L->mods), free(L->order), free(L->plan);
    mt_strs_free(&L->player_order), mt_strs_free(&L->disabled), mt_strs_free(&L->overlay), mt_strs_free(&L->overlay_ids);
    mt_strs_free(&L->refused);
    mt_strs_free(&L->known_ids), mt_strs_free(&L->known_shas), mt_strs_free(&L->affects_ids), mt_strs_free(&L->affects_vals);
    mt_toml_free(L->mappings);
    free(L->mods_dir), free(L->config_dir), free(L->cache_dir), free(L->mappings_dir);
    /* the site table, the event lists and the lock stay: hooks may still be running on game threads */
    free(L);
}

/* ---------------------------------------------------------------- mods/modtopia.toml and the code the player allowed */
static void read_list(MtLoader *L) {
    char *p = mt_path_join(L->mods_dir, "modtopia.toml");
    char *text = mt_read_file(p, NULL);
    if (text) {
        char err[300];
        MtToml *t = mt_toml_parse(text, err, sizeof err);
        if (!t) mt_log(L, MT_LOG_WARN, "%s: %s (the list is ignored)", p, err);
        MtToml *o = mt_toml_get(t, "order"), *d = mt_toml_get(t, "disabled");
        for (int i = 0; o && o->type == MT_T_ARRAY && i < o->n; i++)
            if (o->items[i]->type == MT_T_STRING && mt_strs_find(&L->player_order, o->items[i]->s) < 0) mt_strs_add(&L->player_order, o->items[i]->s);
        for (int i = 0; d && d->type == MT_T_ARRAY && i < d->n; i++)
            if (d->items[i]->type == MT_T_STRING) mt_strs_add(&L->disabled, d->items[i]->s);
        MtToml *af = mt_toml_get(t, "affects");
        for (int i = 0; af && af->type == MT_T_TABLE && i < af->n; i++)
            if (af->items[i]->type == MT_T_STRING && (!strcmp(af->items[i]->s, "gameplay") || !strcmp(af->items[i]->s, "visual")))
                mt_strs_add(&L->affects_ids, af->keys[i]), mt_strs_add(&L->affects_vals, af->items[i]->s);
        mt_toml_free(t);
        free(text);
    }
    free(p);
    p = mt_path_join(L->cache_dir, "known-code.toml");
    text = mt_read_file(p, NULL);
    if (text) {
        MtToml *t = mt_toml_parse(text, NULL, 0), *c = mt_toml_get(t, "code");
        for (int i = 0; c && c->type == MT_T_TABLE && i < c->n; i++)
            if (c->items[i]->type == MT_T_STRING) mt_strs_add(&L->known_ids, c->keys[i]), mt_strs_add(&L->known_shas, c->items[i]->s);
        mt_toml_free(t);
        free(text);
    }
    free(p);
}
int mt_loader_save_list(MtLoader *L) {
    MtBuf b = {0};
    mt_buf_str(&b, "# Modtopia: the order mods load in (first loads first; a later mod's files win) and the mods switched off.\n"
                   "# The launcher, the builder and the game's F1 menu keep this file; changes take effect at the next start.\n");
    mt_buf_str(&b, "order = [");
    for (int i = 0; i < L->player_order.n; i++) {
        mt_buf_str(&b, i ? ", " : "");
        mt_toml_quote(&b, L->player_order.v[i]);
    }
    mt_buf_str(&b, "]\ndisabled = [");
    for (int i = 0; i < L->disabled.n; i++) {
        mt_buf_str(&b, i ? ", " : "");
        mt_toml_quote(&b, L->disabled.v[i]);
    }
    mt_buf_str(&b, "]\n");
    if (L->affects_ids.n) {
        mt_buf_str(&b, "\n# what LayeredFS mods count as for Link Play, where the player decided (\"gameplay\" or \"visual\")\n[affects]\n");
        for (int i = 0; i < L->affects_ids.n; i++) {
            mt_toml_quote(&b, L->affects_ids.v[i]);
            mt_buf_str(&b, " = ");
            mt_toml_quote(&b, L->affects_vals.v[i]);
            mt_buf_str(&b, "\n");
        }
    }
    mt_mkdirs(L->mods_dir);
    char *p = mt_path_join(L->mods_dir, "modtopia.toml");
    int rc = mt_write_file(p, b.p, b.n);
    if (rc) mt_log(L, MT_LOG_ERROR, "cannot write %s", p);
    free(p);
    free(b.p);
    return rc;
}
int mt_loader_trust_code(MtLoader *L) { return mt_loader_trust_mod(L, NULL); }   /* every code mod found now counts as allowed */
/* The same for one mod (an installer's yes for the file it just put in place); others that are new stay new. MT_ERR_UNKNOWN: no mod
   of that id with code here */
int mt_loader_trust_mod(MtLoader *L, const char *only) {
    MtBuf b = {0};
    int found = 0;
    for (int i = 0; only && i < L->nmods; i++) found |= !strcmp(L->mods[i]->m.id, only) && (L->mods[i]->info.flags & MT_MOD_HAS_CODE) && L->mods[i]->sha;
    if (only && !found) return MT_ERR_UNKNOWN;
    mt_buf_str(&b, "# Code mods the player has allowed, by their checksum: a new or changed one is pointed out before it runs.\n[code]\n");
    for (int i = 0; i < L->nmods; i++) {
        Mod *m = L->mods[i];
        if (!(m->info.flags & MT_MOD_HAS_CODE) || !m->sha || (only && strcmp(m->m.id, only))) continue;
        int k = mt_strs_find(&L->known_ids, m->m.id);
        if (k >= 0) mt_strs_remove(&L->known_ids, k), mt_strs_remove(&L->known_shas, k);
        mt_strs_add(&L->known_ids, m->m.id), mt_strs_add(&L->known_shas, m->sha);
        m->info.flags &= ~MT_MOD_NEW_CODE;
    }
    for (int i = 0; i < L->known_ids.n; i++) {
        mt_toml_quote(&b, L->known_ids.v[i]);
        mt_buf_str(&b, " = ");
        mt_toml_quote(&b, L->known_shas.v[i]);
        mt_buf_str(&b, "\n");
    }
    mt_mkdirs(L->cache_dir);
    char *p = mt_path_join(L->cache_dir, "known-code.toml");
    int rc = mt_write_file(p, b.p, b.n);
    free(p);
    free(b.p);
    return rc;
}

/* ---------------------------------------------------------------- discovery */
static Mod *mod_new(MtLoader *L, const char *source) {
    Mod *m = mt_alloc(sizeof *m);
    m->L = L;
    m->source = mt_strdup(source);
    m->info.load_index = -1;
    m->order_pos = INT_MAX;
    L->mods = mt_grow(L->mods, sizeof *L->mods * (size_t)(L->nmods + 1));
    L->mods[L->nmods++] = m;
    return m;
}
static void layered_manifest(Mod *m, const char *display);
static char *id_from_name_pre(const char *name, const char *pre) {   /* an id from a file or folder name */
    MtBuf b = {0};
    mt_buf_str(&b, pre);
    int dash = 0;
    for (const char *c = name; *c && b.n < 60; c++) {
        unsigned char u = (unsigned char)*c;
        if (isalnum(u)) { char l = (char)tolower(u); mt_buf_add(&b, &l, 1); dash = 0; }
        else if (!dash && b.n > 3) { mt_buf_add(&b, "-", 1); dash = 1; }
    }
    size_t k = strlen(pre);
    while (b.n > k && b.p[b.n - 1] == '-') b.p[--b.n] = 0;
    if (b.n == k) mt_buf_str(&b, "data");
    if (!islower((unsigned char)b.p[0])) { char *r = mt_fmt("m%s", b.p); free(b.p); return r; }
    return mt_buf_take(&b);
}
static char *id_from_name(const char *name) { return id_from_name_pre(name, "fs-"); }
static void broken_manifest(Mod *m, const char *display, const char *err) {   /* shown under its folder's name, off with the reason */
    layered_manifest(m, display);
    free(m->m.id);
    m->m.id = id_from_name_pre(display, "");
    m->m.description[0] = 0;
    m->info.flags &= ~(MT_MOD_LAYEREDFS | MT_MOD_HAS_DATA);
    mt_mod_message(m, MT_STATE_ERROR, "%s", err);
}
static void layered_manifest(Mod *m, const char *display) {
    m->m.id = id_from_name(display);
    m->m.name = mt_strdup(display);
    m->m.version = mt_strdup("0.0.0");
    m->m.authors = mt_strdup("");
    m->m.description = mt_strdup("A folder of game files laid over the game (LayeredFS)");
    m->m.license = mt_strdup(""), m->m.homepage = mt_strdup(""), m->m.icon = mt_strdup(""), m->m.loader = mt_strdup("");
    m->m.entry = mt_strdup(""), m->m.data = mt_strdup("");
    m->m.gameplay = 0;
    m->info.flags |= MT_MOD_LAYEREDFS | MT_MOD_HAS_DATA;
    m->layered_name = mt_strdup(display);
}

/* The data root inside a LayeredFS folder: romfs/, one named folder with romfs/ inside, atmosphere/contents/<title>/romfs, or the folder itself */
typedef struct { const char *dir; int ndirs; char *only; int has_romfs, has_exefs, has_atmo; } lf_scan;
static int lf_entry(void *u, const char *name, int is_dir) {
    lf_scan *s = u;
    if (!is_dir) return 0;
    if (!strcmp(name, "romfs")) s->has_romfs = 1;
    else if (!strcmp(name, "exefs")) s->has_exefs = 1;
    else if (!strcmp(name, "atmosphere")) s->has_atmo = 1;
    s->ndirs++;
    free(s->only);
    s->only = mt_strdup(name);
    return 0;
}
static int atmo_entry(void *u, const char *name, int is_dir) {
    char **out = u;
    if (is_dir && !*out) *out = mt_strdup(name);
    return 0;
}
static char *layered_root(MtLoader *L, Mod *m, const char *dir) {
    lf_scan s = {dir, 0, NULL, 0, 0, 0};
    mt_list_dir(dir, lf_entry, &s);
    char *root = NULL;
    if (s.has_romfs) root = mt_path_join(dir, "romfs");
    else if (s.has_atmo) {
        char *c = mt_path_join(dir, "atmosphere/contents"), *title = NULL;
        mt_list_dir(c, atmo_entry, &title);
        if (title) {
            char *t = mt_path_join(c, title), *r = mt_path_join(t, "romfs");
            if (mt_is_dir(r)) root = r; else free(r);
            free(t);
        }
        free(c);
        free(title);
    } else if (s.ndirs == 1 && s.only) {
        char *sub = mt_path_join(dir, s.only), *r = mt_path_join(sub, "romfs");
        if (mt_is_dir(r)) {
            root = r;
            char *ex = mt_path_join(sub, "exefs");
            if (mt_is_dir(ex)) s.has_exefs = 1;
            free(ex);
        } else free(r);
        free(sub);
    }
    if (s.has_exefs) mt_mod_message(m, -1, "its exefs folder (code patches made for emulators) is not used: only its game files are");
    free(s.only);
    (void)L;
    return root ? root : mt_strdup(dir);
}

static char *sha_of_file(const char *path) {
    size_t n;
    char *d = mt_read_file(path, &n);
    if (!d) return NULL;
    char *h = mt_alloc(65);
    mt_sha256_hex(d, n, h);
    free(d);
    return h;
}

static void count_patch(void *u, const char *rel) {
    if (mt_ends_with(rel, ".mtpatch")) ++*(int *)u;
}
typedef struct { MtLoader *L; const char *root; int hit; } refused_t;
static void refused_patch(void *u, const char *rel) {
    refused_t *r = u;
    if (r->hit || !mt_ends_with(rel, ".mtpatch")) return;
    char *p = mt_path_join(r->root, rel), hex[65];
    size_t n = 0;
    char *d = mt_read_file(p, &n);
    if (d) mt_sha256_hex(d, n, hex), r->hit = mt_strs_find(&r->L->refused, hex) >= 0;
    free(d), free(p);
}
static void mod_code_and_data(MtLoader *L, Mod *m) {
    char *bin = mt_path_join(m->dir, "bin");
    if (mt_is_dir(bin)) {
        char *pd = mt_path_join(bin, L->be.platform), *lib = mt_path_join(pd, lib_name(L->be.platform));
        m->info.flags |= MT_MOD_HAS_CODE;
        if (mt_is_file(lib)) {
            m->lib_path = mt_strdup(lib);
            /* a folder mod's checksum covers its code too, so a rebuilt library counts as changed */
            char *ls = sha_of_file(lib);
            if (ls && !(m->info.flags & MT_MOD_LAYEREDFS) && m->sha && mt_ends_with(m->source, ".pkmod") == 0 && mt_ends_with(m->source, ".zip") == 0) {
                char both[130];
                snprintf(both, sizeof both, "%s%s", m->sha, ls);
                mt_sha256_hex(both, strlen(both), m->sha);
            }
            free(ls);
        }
        free(pd);
        free(lib);
    }
    free(bin);
    if (m->m.data && *m->m.data) {
        char *d = mt_path_join(m->dir, m->m.data);
        if (mt_is_dir(d)) { m->data_root = mt_path_abs(d); m->info.flags |= MT_MOD_HAS_DATA; }
        free(d);
    }
    char *pd = mt_path_join(m->dir, "patches");
    if (mt_is_dir(pd)) {
        mt_walk_files(pd, count_patch, &m->npatches);
        if (m->npatches) m->patch_root = mt_path_abs(pd), m->info.flags |= MT_MOD_HAS_PATCHES;
    }
    free(pd);
    if (m->patch_root && L->refused.n) {
        refused_t r = {L, m->patch_root, 0};
        mt_walk_files(m->patch_root, refused_patch, &r);
        if (r.hit) mt_mod_message(m, MT_STATE_ERROR, "refused: a patch of it carries the game's own data, and a patch may carry only the change");
    }
    if (m->m.gameplay) m->info.flags |= MT_MOD_GAMEPLAY;
    if ((m->info.flags & MT_MOD_HAS_CODE) && m->sha) {
        int k = mt_strs_find(&L->known_ids, m->m.id);
        if (k < 0 || strcmp(L->known_shas.v[k], m->sha)) m->info.flags |= MT_MOD_NEW_CODE;
    }
}

static void from_folder(MtLoader *L, const char *name, const char *path) {
    Mod *m = mod_new(L, name);
    char *mt = mt_path_join(path, "mod.toml");
    char *text = mt_read_file(mt, NULL);
    m->dir = mt_path_abs(path);
    if (text) {
        char err[400];
        m->sha = mt_alloc(65);
        mt_sha256_hex(text, strlen(text), m->sha);
        if (mt_manifest_parse(text, &m->m, err, sizeof err)) {
            broken_manifest(m, name, err);
        } else {
            m->has_manifest = 1;
            mod_code_and_data(L, m);
        }
    } else {
        layered_manifest(m, name);
        m->data_root = layered_root(L, m, path);
        char *abs = mt_path_abs(m->data_root);
        free(m->data_root);
        m->data_root = abs;
    }
    free(text);
    free(mt);
    mt_info_refresh(m);
}

/* the folder inside a zip that holds mod.toml: "" or "<one folder>/" */
static const char *zip_manifest_prefix(const MtZip *z, int *at) {
    static char pre[512];
    if ((*at = mt_zip_find(z, "mod.toml")) >= 0) return "";
    for (int i = 0; i < z->n; i++) {
        const char *s = strchr(z->e[i].name, '/');
        if (s && !strcmp(s + 1, "mod.toml") && (size_t)(s - z->e[i].name) + 2 < sizeof pre) {
            snprintf(pre, sizeof pre, "%.*s/", (int)(s - z->e[i].name), z->e[i].name);
            *at = i;
            return pre;
        }
    }
    *at = -1;
    return NULL;
}
static int mark_used(MtStrs *used, const char *name) { mt_strs_add(used, name); return 0; }

static void from_zip(MtLoader *L, const char *name, const char *path, MtStrs *used_cache) {
    Mod *m = mod_new(L, name);
    MtZip z;
    char err[400];
    if (mt_zip_open(&z, path, err, sizeof err)) {
        broken_manifest(m, name, err);
        mt_info_refresh(m);
        return;
    }
    m->sha = mt_alloc(65);
    mt_sha256_hex(z.data, z.size, m->sha);
    int at;
    const char *prefix = zip_manifest_prefix(&z, &at);
    char *dirname = NULL;
    if (prefix) {
        size_t n;
        uint8_t *text = mt_zip_read(&z, at, &n, err, sizeof err);
        if (!text || mt_manifest_parse((const char *)text, &m->m, err, sizeof err)) {
            free(text);
            broken_manifest(m, name, err);
            mt_zip_close(&z);
            mt_info_refresh(m);
            return;
        }
        free(text);
        m->has_manifest = 1;
        dirname = mt_fmt("%s-%s-%.12s", m->m.id, m->m.version, m->sha);
    } else {
        layered_manifest(m, name);
        prefix = "";
        char *base = mt_strdup(name);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        free(m->m.id), free(m->m.name), free(m->layered_name);
        m->m.id = id_from_name(base), m->m.name = mt_strdup(base), m->layered_name = mt_strdup(base);
        free(base);
        dirname = mt_fmt("%s-%.12s", m->m.id, m->sha);
    }
    char *dir = mt_path_join(L->cache_dir, dirname), *done = mt_path_join(dir, ".complete");
    mark_used(used_cache, dirname);
    if (!mt_is_file(done)) {   /* unpacked once per checksum */
        mt_remove_tree(dir);
        if (mt_zip_extract(&z, prefix, dir, err, sizeof err)) {
            mt_mod_message(m, MT_STATE_ERROR, "cannot unpack: %s", err);
            mt_remove_tree(dir);
        } else {
            mt_write_file(done, "", 0);
        }
    }
    mt_zip_close(&z);
    m->dir = mt_path_abs(dir);
    if (m->info.state != MT_STATE_ERROR) {
        if (m->has_manifest) mod_code_and_data(L, m);
        else {
            char *r = layered_root(L, m, dir);
            m->data_root = mt_path_abs(r);
            free(r);
        }
    }
    free(done);
    free(dir);
    free(dirname);
    mt_info_refresh(m);
}

typedef struct { MtLoader *L; MtStrs used; } disc_t;
static int disc_entry(void *u, const char *name, int is_dir) {
    disc_t *d = u;
    if (name[0] == '.' || !strcmp(name, "modtopia.toml")) return 0;
    char *p = mt_path_join(d->L->mods_dir, name);
    if (is_dir) from_folder(d->L, name, p);
    else if (mt_ends_with(name, ".pkmod") || mt_ends_with(name, ".zip")) from_zip(d->L, name, p, &d->used);
    free(p);
    return 0;
}
typedef struct { MtLoader *L; MtStrs *used; } clean_t;
static int clean_entry(void *u, const char *name, int is_dir) {
    clean_t *c = u;
    if (!is_dir || mt_strs_find(c->used, name) >= 0) return 0;
    char *p = mt_path_join(c->L->cache_dir, name);
    mt_remove_tree(p);   /* an older unpacked copy */
    free(p);
    return 0;
}

/* A LayeredFS mod's kind for Link Play: visual only when every file is a kind that only changes how the game looks or sounds; anything
   else (data tables, archives, scripts, unknown kinds) can change what happens, so gameplay. The player's word in [affects] wins */
static const char *const VISUAL_EXT[] = {".webm", ".mp4", ".bk2", ".usm", ".bntx", ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bflim", ".bclim", ".bfres",
                                         ".bfsha", ".bfttf", ".bfotf", ".bffnt", ".ttf", ".otf", ".bfstm", ".bfwav", ".bwav", ".bwar", ".bars", ".wem",
                                         ".ogg", ".opus", ".wav", ".bflyt", ".bflan", ".bclyt", ".bclan", NULL};
typedef struct { int total, visual; } kind_count;
static void kind_file(void *u, const char *rel) {
    kind_count *k = u;
    k->total++;
    for (int i = 0; VISUAL_EXT[i]; i++)
        if (mt_ends_with(rel, VISUAL_EXT[i])) { k->visual++; return; }
}
static void layered_kind(MtLoader *L, Mod *m) {
    int at = mt_strs_find(&L->affects_ids, m->m.id);
    m->info.flags &= ~(MT_MOD_GAMEPLAY | MT_MOD_KIND_GUESSED);
    if (at >= 0) {
        if (!strcmp(L->affects_vals.v[at], "gameplay")) m->info.flags |= MT_MOD_GAMEPLAY;
        return;
    }
    kind_count k = {0, 0};
    if (m->data_root) mt_walk_files(m->data_root, kind_file, &k);
    m->info.flags |= MT_MOD_KIND_GUESSED;
    if (!k.total || k.visual < k.total) m->info.flags |= MT_MOD_GAMEPLAY;
}
int mt_loader_set_affects(MtLoader *L, const char *id, const char *affects) {
    if (affects && strcmp(affects, "gameplay") && strcmp(affects, "visual")) return MT_ERR_UNKNOWN;
    int at = mt_strs_find(&L->affects_ids, id);
    if (at >= 0) mt_strs_remove(&L->affects_ids, at), mt_strs_remove(&L->affects_vals, at);
    if (affects) mt_strs_add(&L->affects_ids, id), mt_strs_add(&L->affects_vals, affects);
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(L->mods[i]->m.id, id) && (L->mods[i]->info.flags & MT_MOD_LAYEREDFS)) layered_kind(L, L->mods[i]);
    return mt_loader_save_list(L);
}

/* ---------------------------------------------------------------- mappings */
static void load_mappings(MtLoader *L) {
    if (!L->mappings_dir || !L->be.game_version) return;
    char *f = mt_fmt("%s.toml", L->be.game_version), *p = mt_path_join(L->mappings_dir, f);
    char *text = mt_read_file(p, NULL);
    if (text) {
        char err[300];
        L->mappings = mt_toml_parse(text, err, sizeof err);
        if (!L->mappings) mt_log(L, MT_LOG_WARN, "the mappings %s cannot be read: %s", p, err);
        else {
            const char *b = mt_toml_str(L->mappings, "build_id", NULL);
            if (b && L->be.game_build_id && strncmp(b, L->be.game_build_id, strlen(b))) {
                mt_log(L, MT_LOG_WARN, "the mappings %s are for main %s, the game is %s: not used", p, b, L->be.game_build_id);
                mt_toml_free(L->mappings);
                L->mappings = NULL;
            }
        }
    }
    free(text);
    free(p);
    free(f);
}
/* "main+0x1234", or a mapped function or global name: module and offset (and the mapping's word); 0 when unknown */
uint64_t mt_resolve_name(MtLoader *L, const char *name, uint32_t *word, char module[16], uint64_t *offset) {
    const char *plus = strchr(name, '+');
    char mod[16] = "";
    uint64_t off = 0;
    if (word) *word = 0;
    if (plus && (size_t)(plus - name) < sizeof mod) {
        memcpy(mod, name, (size_t)(plus - name));
        mod[plus - name] = 0;
        char *e;
        off = strtoull(plus + 1, &e, 0);
        if (*e) return 0;
    } else {
        MtToml *f = mt_toml_get(mt_toml_get(L->mappings, "functions"), name);
        MtToml *g = mt_toml_get(mt_toml_get(L->mappings, "globals"), name);
        const char *at = f && f->type == MT_T_TABLE ? mt_toml_str(f, "at", NULL) : f && f->type == MT_T_STRING ? f->s : g && g->type == MT_T_STRING ? g->s : NULL;
        if (!at) return 0;
        if (f && f->type == MT_T_TABLE && word) *word = (uint32_t)mt_toml_int(f, "word", 0);
        return mt_resolve_name(L, at, NULL, module, offset) ? 1 : 0;
    }
    if (strcmp(mod, "main") && strcmp(mod, "sdk") && strcmp(mod, "subsdk0") && strcmp(mod, "rtld")) return 0;
    if (module) snprintf(module, 16, "%s", mod);
    if (offset) *offset = off;
    return 1;
}
const char *mt_mapping_area_name(MtLoader *L, int32_t area) {
    char k[16];
    snprintf(k, sizeof k, "%d", area);
    return mt_toml_str(mt_toml_get(L->mappings, "areas"), k, NULL);
}

/* ---------------------------------------------------------------- resolution */
static Mod *find_ok(MtLoader *L, const char *id) {
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(L->mods[i]->m.id, id) && L->mods[i]->info.state == MT_STATE_OK) return L->mods[i];
    return NULL;
}
static Mod *find_any(MtLoader *L, const char *id) {
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(L->mods[i]->m.id, id)) return L->mods[i];
    return NULL;
}

static void check_mod(MtLoader *L, Mod *m) {
    if (!m->has_manifest) return;   /* LayeredFS data: nothing to check */
    MtManifest *x = &m->m;
    if (x->loader[0] && mt_version_satisfies(MT_CORE_VERSION, x->loader) != 1) {
        mt_mod_message(m, MT_STATE_ERROR, "needs Modtopia %s; this is %s", x->loader, MT_CORE_VERSION);
        return;
    }
    if (L->be.game_version && x->games.n && mt_strs_find(&x->games, L->be.game_version) < 0) {
        mt_mod_message(m, MT_STATE_OFF, "made for game version%s %s, not %s", x->games.n > 1 ? "s" : "", x->games.v[0], L->be.game_version);
        return;
    }
    if (x->platforms.n && mt_strs_find(&x->platforms, L->be.platform) < 0) {
        mt_mod_message(m, MT_STATE_OFF, "not made for %s", L->be.platform);
        return;
    }
    if ((m->info.flags & MT_MOD_HAS_CODE) && !m->lib_path) {
        mt_mod_message(m, MT_STATE_OFF, "has no code for %s (bin/%s/%s)", L->be.platform, L->be.platform, lib_name(L->be.platform));
        return;
    }
    if (x->nhooks && L->be.game_version) {
        const MtHookSet *set = mt_manifest_hooks_for(x, L->be.game_version);
        if (!set) {
            mt_mod_message(m, MT_STATE_OFF, "has no hook points for game version %s", L->be.game_version);
            return;
        }
        m->pts = mt_alloc(sizeof *m->pts * (size_t)(set->n ? set->n : 1));
        for (int i = 0; i < set->n; i++) {
            const MtHookDecl *h = &set->h[i];
            MtPoint *p = &m->pts[m->npts++];
            uint32_t mword = 0;
            p->name = mt_strdup(h->name);
            p->kind = h->kind;
            if (!mt_resolve_name(L, h->target, &mword, p->module, &p->offset)) {
                mt_mod_message(m, MT_STATE_OFF, "the hook point '%s' names %s, which the mappings for %s do not have", h->name, h->target, L->be.game_version);
                return;
            }
            p->offset += (uint64_t)h->offset;
            if (h->word >= 0) p->word = (uint32_t)h->word;
            else if (!h->offset && mword) p->word = mword;
            else {
                mt_mod_message(m, MT_STATE_OFF, "the hook point '%s' has no expected instruction word", h->name);
                return;
            }
        }
    }
}

static int dep_ok(MtLoader *L, Mod *m) {   /* 0 when a required dependency is missing, off or at a wrong version */
    for (int i = 0; i < m->m.ndeps; i++) {
        MtDep *d = &m->m.deps[i];
        Mod *t = find_ok(L, d->id);
        if (t && d->range[0] && mt_version_satisfies(t->m.version, d->range) != 1) {
            if (!d->required) continue;
            mt_mod_message(m, MT_STATE_ERROR, "needs %s %s; %s is there", d->id, d->range, t->m.version);
            return 0;
        }
        if (!t && d->required) {
            Mod *any = find_any(L, d->id);
            mt_mod_message(m, MT_STATE_ERROR, "needs %s%s%s, which is %s", d->id, d->range[0] ? " " : "", d->range,
                           !any ? "not in the mods folder" : !any->info.enabled ? "switched off" : "not working");
            return 0;
        }
    }
    return 1;
}

/* Kahn's order: each edge a -> b means a loads before b; ties go to the player's order, then the id */
static int order_less(const Mod *a, const Mod *b) {
    if (a->order_pos != b->order_pos) return a->order_pos < b->order_pos;
    return strcmp(a->m.id, b->m.id) < 0;
}
static void build_order(MtLoader *L) {
    int n = 0;
    Mod **ok = mt_alloc(sizeof *ok * (size_t)(L->nmods + 1));
    for (int i = 0; i < L->nmods; i++)
        if (L->mods[i]->info.state == MT_STATE_OK) ok[n++] = L->mods[i];
    int *indeg = mt_alloc(sizeof *indeg * (size_t)(n + 1));
    uint8_t *edge = mt_alloc((size_t)(n * n + 1));
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ok[i]->m.ndeps; k++) {
            MtDep *d = &ok[i]->m.deps[k];
            for (int j = 0; j < n; j++) {
                if (strcmp(ok[j]->m.id, d->id) || !d->order) continue;
                int a = d->order > 0 ? j : i, b = d->order > 0 ? i : j;
                if (!edge[a * n + b]) edge[a * n + b] = 1, indeg[b]++;
            }
        }
    L->order = mt_grow(L->order, sizeof *L->order * (size_t)(n + 1));
    L->norder = 0;
    uint8_t *done = mt_alloc((size_t)n + 1);
    for (int step = 0; step < n; step++) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (!done[i] && !indeg[i] && (best < 0 || order_less(ok[i], ok[best]))) best = i;
        if (best < 0) break;
        done[best] = 1;
        L->order[L->norder++] = ok[best];
        for (int j = 0; j < n; j++)
            if (edge[best * n + j]) indeg[j]--;
    }
    for (int i = 0; i < n; i++)
        if (!done[i]) mt_mod_message(ok[i], MT_STATE_ERROR, "its load order cannot be met: a loop of before/after with another mod");
    for (int i = 0; i < L->norder; i++) L->order[i]->info.load_index = i;
    free(done), free(edge), free(indeg), free(ok);
}

static void resolve(MtLoader *L) {
    /* the player's switches and order; the same id twice keeps the newer version */
    for (int i = 0; i < L->nmods; i++) {
        Mod *m = L->mods[i];
        m->info.enabled = mt_strs_find(&L->disabled, m->m.id) < 0;
        m->order_pos = mt_strs_find(&L->player_order, m->m.id);
        if (m->order_pos < 0) m->order_pos = INT_MAX;
        if (m->info.state != MT_STATE_OK) continue;
        for (int j = 0; j < i; j++) {
            Mod *o = L->mods[j];
            if (strcmp(o->m.id, m->m.id) || o->info.state == MT_STATE_ERROR) continue;
            Mod *loser = mt_version_compare(o->m.version, m->m.version) >= 0 ? m : o, *winner = loser == m ? o : m;
            mt_mod_message(loser, MT_STATE_ERROR, "the same mod as %s (%s): only one copy loads", winner->source, winner->m.version);
        }
    }
    for (int i = 0; i < L->nmods; i++) {
        Mod *m = L->mods[i];
        if (m->info.state != MT_STATE_OK) continue;
        if (!m->info.enabled) { mt_mod_message(m, MT_STATE_OFF, "switched off"); continue; }
        check_mod(L, m);
    }
    for (int changed = 1; changed;) {   /* a mod that fails can take its dependents with it */
        changed = 0;
        for (int i = 0; i < L->nmods; i++)
            if (L->mods[i]->info.state == MT_STATE_OK && !dep_ok(L, L->mods[i])) changed = 1;
    }
    build_order(L);
    /* replace: one mod per function; a point's instruction word must agree between mods */
    for (int a = 0; a < L->norder; a++)
        for (int b = a + 1; b < L->norder; b++) {
            Mod *x = L->order[a], *y = L->order[b];
            if (x->info.state != MT_STATE_OK || y->info.state != MT_STATE_OK) continue;
            for (int i = 0; i < x->npts; i++)
                for (int j = 0; j < y->npts; j++) {
                    MtPoint *p = &x->pts[i], *q = &y->pts[j];
                    if (strcmp(p->module, q->module) || p->offset != q->offset) continue;
                    if (p->word != q->word)
                        mt_mod_message(y, MT_STATE_ERROR, "expects another instruction at %s+%#llx than %s does (%08x, not %08x)", q->module,
                                       (unsigned long long)q->offset, x->m.id, q->word, p->word);
                    else if (p->kind == MT_HOOK_REPLACE && q->kind == MT_HOOK_REPLACE)
                        mt_mod_message(y, MT_STATE_ERROR, "replaces %s+%#llx, which %s replaces already: only one mod may", q->module, (unsigned long long)q->offset, x->m.id);
                }
        }
    /* those errors change the order's members: build it again */
    for (int i = 0; i < L->nmods; i++) L->mods[i]->info.load_index = -1;
    build_order(L);
}

/* ---------------------------------------------------------------- settings */
static void config_load(MtLoader *L, Mod *m) {
    if (!m->has_manifest || !m->m.nsettings) return;
    m->cfg = mt_alloc(sizeof *m->cfg * (size_t)m->m.nsettings);
    for (int i = 0; i < m->m.nsettings; i++) {
        m->cfg[i].num = m->m.settings[i].def_num;
        m->cfg[i].text = mt_strdup(m->m.settings[i].def_text ? m->m.settings[i].def_text : "");
    }
    char *f = mt_fmt("%s.toml", m->m.id), *p = mt_path_join(L->config_dir, f);
    char *text = mt_read_file(p, NULL);
    if (text) {
        char err[300];
        MtToml *t = mt_toml_parse(text, err, sizeof err);
        if (!t) mt_log(L, MT_LOG_WARN, "%s: %s (its defaults are used)", p, err);
        for (int i = 0; t && i < m->m.nsettings; i++) {
            MtSetting *s = &m->m.settings[i];
            MtToml *v = mt_toml_get(t, s->info.key);
            if (!v) continue;
            int okv = 1;
            switch (s->info.type) {
            case MT_SET_BOOL: if (v->type == MT_T_BOOL) m->cfg[i].num = v->b; else okv = 0; break;
            case MT_SET_NUMBER:
                if (v->type == MT_T_INT || v->type == MT_T_FLOAT) {
                    double d = v->type == MT_T_INT ? (double)v->i : v->f;
                    m->cfg[i].num = d < s->info.min ? s->info.min : d > s->info.max ? s->info.max : d;
                } else okv = 0;
                break;
            case MT_SET_CHOICE: {
                int k = v->type == MT_T_STRING ? mt_strs_find(&s->choices, v->s) : -1;
                if (k >= 0) m->cfg[i].num = k; else okv = 0;
                break;
            }
            default:
                if (v->type == MT_T_STRING) { free(m->cfg[i].text); m->cfg[i].text = mt_strdup(v->s); } else okv = 0;
            }
            if (!okv) mt_log(L, MT_LOG_WARN, "%s: '%s' is not a valid value for this setting (the default is used)", p, s->info.key);
        }
        mt_toml_free(t);
    }
    free(text);
    free(p);
    free(f);
}
static int config_save(MtLoader *L, Mod *m) {
    MtBuf b = {0};
    mt_buf_fmt(&b, "# %s %s: your settings (the game's F1 menu and the launchers change this file too)\n", m->m.name, m->m.version);
    for (int i = 0; i < m->m.nsettings; i++) {
        MtSetting *s = &m->m.settings[i];
        mt_buf_str(&b, "\n# ");
        mt_buf_str(&b, s->info.label);
        if (s->info.type == MT_SET_NUMBER) mt_buf_fmt(&b, " (%g to %g)", s->info.min, s->info.max);
        if (s->info.type == MT_SET_CHOICE) {
            mt_buf_str(&b, ":");
            for (int k = 0; k < s->choices.n; k++) mt_buf_fmt(&b, " %s", s->choices.v[k]);
        }
        mt_buf_fmt(&b, "\n%s = ", s->info.key);
        switch (s->info.type) {
        case MT_SET_BOOL: mt_buf_str(&b, m->cfg[i].num ? "true" : "false"); break;
        case MT_SET_NUMBER: {
            double d = m->cfg[i].num;
            if (d == floor(d) && fabs(d) < 1e15) mt_buf_fmt(&b, "%.1f", d); else mt_buf_fmt(&b, "%.17g", d);
            break;
        }
        case MT_SET_CHOICE: mt_toml_quote(&b, s->choices.v[(int)m->cfg[i].num]); break;
        default: mt_toml_quote(&b, m->cfg[i].text);
        }
        mt_buf_str(&b, "\n");
    }
    mt_mkdirs(L->config_dir);
    char *f = mt_fmt("%s.toml", m->m.id), *p = mt_path_join(L->config_dir, f);
    int rc = mt_write_file(p, b.p, b.n);
    if (rc) mt_log(L, MT_LOG_ERROR, "cannot write %s", p);
    free(p), free(f), free(b.p);
    return rc;
}
int mt_setting_count(MtLoader *L, int mod) {
    Mod *m = mod >= 0 && mod < L->nmods ? listed(L, mod) : NULL;
    return m && m->has_manifest ? m->m.nsettings : 0;
}
const MtSettingInfo *mt_setting_info(MtLoader *L, int mod, int i) {
    return i >= 0 && i < mt_setting_count(L, mod) ? &listed(L, mod)->m.settings[i].info : NULL;
}
double mt_setting_get_number(MtLoader *L, int mod, int i) {
    if (i < 0 || i >= mt_setting_count(L, mod) || !listed(L, mod)->cfg) return 0;
    return listed(L, mod)->cfg[i].num;
}
int mt_setting_get_text(MtLoader *L, int mod, int i, char *out, size_t n) {
    if (i < 0 || i >= mt_setting_count(L, mod) || !listed(L, mod)->cfg) return MT_ERR_UNKNOWN;
    Mod *m = listed(L, mod);
    MtSetting *s = &m->m.settings[i];
    switch (s->info.type) {
    case MT_SET_BOOL: snprintf(out, n, "%s", m->cfg[i].num ? "On" : "Off"); break;
    case MT_SET_NUMBER: snprintf(out, n, "%g", m->cfg[i].num); break;
    case MT_SET_CHOICE: snprintf(out, n, "%s", s->choices.v[(int)m->cfg[i].num]); break;
    default: snprintf(out, n, "%s", m->cfg[i].text);
    }
    return MT_OK;
}
int mt_setting_set_number(MtLoader *L, int mod, int i, double v) {
    if (i < 0 || i >= mt_setting_count(L, mod) || !listed(L, mod)->cfg) return MT_ERR_UNKNOWN;
    Mod *m = listed(L, mod);
    MtSetting *s = &m->m.settings[i];
    if (s->info.type == MT_SET_BOOL) v = v ? 1 : 0;
    else if (s->info.type == MT_SET_NUMBER) {
        if (s->info.step > 0) v = s->info.min + floor((v - s->info.min) / s->info.step + 0.5) * s->info.step;
        v = v < s->info.min ? s->info.min : v > s->info.max ? s->info.max : v;
    } else if (s->info.type == MT_SET_CHOICE) {
        int k = (int)v;
        v = k < 0 ? 0 : k >= s->choices.n ? s->choices.n - 1 : k;
    } else return MT_ERR_STATE;
    mt_lock(&L->lock);
    m->cfg[i].num = v;
    mt_unlock(&L->lock);
    config_save(L, m);
    mt_config_notify(m, s->info.key);
    return MT_OK;
}
int mt_setting_set_text(MtLoader *L, int mod, int i, const char *v) {
    if (i < 0 || i >= mt_setting_count(L, mod) || !listed(L, mod)->cfg) return MT_ERR_UNKNOWN;
    Mod *m = listed(L, mod);
    MtSetting *s = &m->m.settings[i];
    if (s->info.type == MT_SET_CHOICE) {
        int k = mt_strs_find(&s->choices, v);
        return k < 0 ? MT_ERR_UNKNOWN : mt_setting_set_number(L, mod, i, k);
    }
    if (s->info.type != MT_SET_TEXT && s->info.type != MT_SET_KEY) return MT_ERR_STATE;
    mt_lock(&L->lock);
    char *old = m->cfg[i].text;   /* kept: a handler may still hold it */
    m->cfg[i].text = mt_strdup(v);
    mt_unlock(&L->lock);
    (void)old;
    config_save(L, m);
    mt_config_notify(m, s->info.key);
    return MT_OK;
}

/* ---------------------------------------------------------------- the plan */
static int plan_cmp(const void *a, const void *b) {
    const MtPlanPoint *x = a, *y = b;
    int c = strcmp(x->module, y->module);
    return c ? c : x->offset < y->offset ? -1 : x->offset > y->offset;
}
static const char *kind_name(int k) { return k == MT_HOOK_REPLACE ? "replace" : k == MT_HOOK_TAKE_OVER ? "take_over" : "observe"; }
static void build_plan(MtLoader *L) {
    free(L->plan);
    L->plan = NULL;
    L->nplan = 0;
    int cap = 0;
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK) continue;
        for (int k = 0; k < m->npts; k++) {
            MtPoint *p = &m->pts[k];
            int j;
            for (j = 0; j < L->nplan; j++)
                if (!strcmp(L->plan[j].module, p->module) && L->plan[j].offset == p->offset) break;
            if (j == L->nplan) {
                if (L->nplan == cap) L->plan = mt_grow(L->plan, sizeof *L->plan * (size_t)(cap = cap ? cap * 2 : 16));
                memset(&L->plan[j], 0, sizeof L->plan[j]);
                snprintf(L->plan[j].module, sizeof L->plan[j].module, "%s", p->module);
                L->plan[j].offset = p->offset;
                L->plan[j].word = p->word;
                L->nplan++;
            }
            if (p->kind > L->plan[j].kind) L->plan[j].kind = p->kind;
        }
    }
    if (L->nplan) qsort(L->plan, (size_t)L->nplan, sizeof *L->plan, plan_cmp);
    MtBuf b = {0};
    for (int i = 0; i < L->nplan; i++)
        mt_buf_fmt(&b, "%s\t%#llx\t%s\t%#010x\n", L->plan[i].module, (unsigned long long)L->plan[i].offset, kind_name(L->plan[i].kind), L->plan[i].word);
    if (b.n) mt_sha256_hex(b.p, b.n, L->plan_hash);
    else L->plan_hash[0] = 0;
    free(b.p);
}
int mt_plan_count(MtLoader *L) { return L->nplan; }
const MtPlanPoint *mt_plan_point(MtLoader *L, int i) { return i >= 0 && i < L->nplan ? &L->plan[i] : NULL; }
const char *mt_plan_hash(MtLoader *L) { return L->plan_hash; }
int mt_plan_write(MtLoader *L, const char *path) {
    MtBuf b = {0};
    mt_buf_fmt(&b, "# modtopia hooks 1\n# game %s\n# hash %s\n# module\toffset\tkind\tword\n", L->be.game_version ? L->be.game_version : "?", L->plan_hash);
    for (int i = 0; i < L->nplan; i++)
        mt_buf_fmt(&b, "%s\t%#llx\t%s\t%#010x\n", L->plan[i].module, (unsigned long long)L->plan[i].offset, kind_name(L->plan[i].kind), L->plan[i].word);
    int rc = mt_write_file(path, b.p ? b.p : "", b.n);
    free(b.p);
    return rc;
}

/* ---------------------------------------------------------------- scan */
/* the overlay: the last mod loaded wins, so it comes first; a mod's own files win over its patched ones */
static void overlay_build(MtLoader *L) {
    mt_strs_free(&L->overlay), mt_strs_free(&L->overlay_ids);
    for (int i = L->norder - 1; i >= 0; i--) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK) continue;
        if (m->data_root) mt_strs_add(&L->overlay, m->data_root), mt_strs_add(&L->overlay_ids, m->m.id);
        if (m->patched_root) mt_strs_add(&L->overlay, m->patched_root), mt_strs_add(&L->overlay_ids, m->m.id);
    }
}

int mt_loader_scan(MtLoader *L) {
    read_list(L);
    load_mappings(L);
    {   /* patches refused earlier (mt_loader_apply_patches): their mods stay off */
        char *p = mt_path_join(L->cache_dir, "patched/refused.txt"), *t = mt_read_file(p, NULL);
        for (char *s = t; s && *s;) {
            char *e = strchr(s, '\n');
            size_t len = e ? (size_t)(e - s) : strlen(s);
            if (len >= 64) {
                char h[65];
                memcpy(h, s, 64), h[64] = 0;
                if (mt_strs_find(&L->refused, h) < 0) mt_strs_add(&L->refused, h);
            }
            s = e ? e + 1 : s + len;
        }
        free(t), free(p);
    }
    if (!mt_is_dir(L->mods_dir)) {
        mt_log(L, MT_LOG_INFO, "no mods folder (%s): nothing to load", L->mods_dir);
        return -1;
    }
    disc_t d = {L, {0}};
    mt_list_dir(L->mods_dir, disc_entry, &d);
    for (int i = 0; i < L->nmods; i++)
        if (L->mods[i]->info.flags & MT_MOD_LAYEREDFS) layered_kind(L, L->mods[i]);
    mt_strs_add(&d.used, "patched");   /* patched game files: mt_loader_apply_patches keeps that folder tidy */
    clean_t c = {L, &d.used};
    if (mt_is_dir(L->cache_dir)) mt_list_dir(L->cache_dir, clean_entry, &c);
    mt_strs_free(&d.used);
    resolve(L);
    for (int i = 0; i < L->nmods; i++) config_load(L, L->mods[i]);
    build_plan(L);
    overlay_build(L);
    for (int i = 0; i < L->nmods; i++) mt_info_refresh(L->mods[i]);
    /* new mods join the player's order at its end, so the list file shows everything */
    int added = 0;
    for (int i = 0; i < L->norder; i++)
        if (mt_strs_find(&L->player_order, L->order[i]->m.id) < 0) mt_strs_add(&L->player_order, L->order[i]->m.id), added = 1;
    (void)added;
    mt_sites_build(L);
    return 0;
}

/* ---------------------------------------------------------------- a mod list */
int mt_loader_count(MtLoader *L) { return L->nmods; }
static Mod *listed(MtLoader *L, int i) {   /* the load order first, then the rest as found */
    if (i < L->norder) return L->order[i];
    int k = L->norder;
    for (int j = 0; j < L->nmods; j++) {
        if (L->mods[j]->info.load_index >= 0) continue;
        if (k++ == i) return L->mods[j];
    }
    return NULL;
}
const MtModInfo *mt_loader_info(MtLoader *L, int i) {
    Mod *m = i >= 0 && i < L->nmods ? listed(L, i) : NULL;
    return m ? &m->info : NULL;
}
int mt_loader_find(MtLoader *L, const char *id) {
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(listed(L, i)->m.id, id)) return i;
    return -1;
}
int mt_loader_set_enabled(MtLoader *L, const char *id, int on) {
    int k = mt_strs_find(&L->disabled, id);
    if (on && k >= 0) mt_strs_remove(&L->disabled, k);
    if (!on && k < 0) mt_strs_add(&L->disabled, id);
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(L->mods[i]->m.id, id)) L->mods[i]->info.enabled = on;
    if (mt_strs_find(&L->player_order, id) < 0) mt_strs_add(&L->player_order, id);
    return mt_loader_save_list(L);
}
int mt_loader_move(MtLoader *L, const char *id, int delta) {
    int k = mt_strs_find(&L->player_order, id);
    if (k < 0) { mt_strs_add(&L->player_order, id); k = L->player_order.n - 1; }
    int to = k + delta;
    if (to < 0 || to >= L->player_order.n) return MT_ERR_STATE;
    char *s = L->player_order.v[k];
    L->player_order.v[k] = L->player_order.v[to];
    L->player_order.v[to] = s;
    return mt_loader_save_list(L);
}

int mt_loader_gameplay_mods(MtLoader *L, void (*fn)(void *user, const char *id_at_version), void *user) {
    int n = 0;
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK || !(m->info.flags & MT_MOD_GAMEPLAY)) continue;
        if ((m->info.flags & MT_MOD_HAS_CODE) && !m->info.started && L->started) continue;
        char *s = mt_fmt("%s@%s", m->m.id, m->m.version);
        if (fn) fn(user, s);
        free(s);
        n++;
    }
    return n;
}

int mt_loader_overlay_count(MtLoader *L) { return L->overlay.n; }
const char *mt_loader_overlay_root(MtLoader *L, int i) { return i >= 0 && i < L->overlay.n ? L->overlay.v[i] : NULL; }
const char *mt_loader_overlay_mod(MtLoader *L, int i) { return i >= 0 && i < L->overlay_ids.n ? L->overlay_ids.v[i] : NULL; }

typedef struct { MtLoader *L; int total, replaced; char *note; } data_count;
static void data_file(void *u, const char *rel) {
    data_count *c = u;
    c->total++;
    if (c->L->be.game_has_file(c->L->be.user, rel)) c->replaced++;
    if (!c->note && c->L->be.file_note) {
        const char *n = c->L->be.file_note(c->L->be.user, rel);
        if (n) c->note = mt_fmt("%s %s", rel, n);
    }
}
void mt_loader_check_data(MtLoader *L) {
    if (!L->be.game_has_file) return;
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK || !m->data_root) continue;
        data_count c = {L, 0, 0, NULL};
        mt_walk_files(m->data_root, data_file, &c);
        if (c.total && !c.replaced) {
            mt_log(L, MT_LOG_WARN, "%s: none of its %d files is one of the game's: its game files go at its top (or in romfs/), so the folder level may be wrong",
                   m->m.id, c.total);
            mt_loader_note(L, m->m.id, "none of its files is one of the game's (the wrong folder level?)");
        } else {
            mt_log(L, MT_LOG_INFO, "%s: %d file%s over the game's, %d new", m->m.id, c.replaced, c.replaced == 1 ? "" : "s", c.total - c.replaced);
        }
        if (c.note) {
            mt_log(L, MT_LOG_WARN, "%s: %s", m->m.id, c.note);
            mt_loader_note(L, m->m.id, c.note);
            free(c.note);
        }
    }
}

/* ---------------------------------------------------------------- patches (SPEC §10): applied to the player's own game files */
typedef struct { MtLoader *L; const char *rom; } game_src;
static int64_t game_src_read(void *u, uint64_t off, void *buf, uint64_t n) {
    game_src *g = u;
    return g->L->be.game_read(g->L->be.user, g->rom, off, buf, n);
}
static void list_patch(void *u, const char *rel) {
    if (mt_ends_with(rel, ".mtpatch")) mt_strs_add(u, rel);
}
typedef struct { MtLoader *L; MtStrs *keep, *active; } tidy_t;
/* .modtopia/patched/<id>-<key>: removed when that mod patches now under another key, or is no longer there (a mod switched off keeps its
   folder, so switching it on again needs no new patching) */
static int tidy_patched(void *u, const char *name, int is_dir) {
    tidy_t *t = u;
    const char *dash = strrchr(name, '-');
    if (!is_dir || !dash || mt_strs_find(t->keep, name) >= 0) return 0;
    char *id = mt_strndup(name, (size_t)(dash - name));
    int there = 0;
    for (int i = 0; i < t->L->nmods; i++) there |= !strcmp(t->L->mods[i]->m.id, id);
    if (mt_strs_find(t->active, id) >= 0 || !there) {
        char *p = mt_path_join(t->L->cache_dir, "patched"), *q = mt_path_join(p, name);
        mt_remove_tree(q);
        free(p), free(q);
    }
    free(id);
    return 0;
}
static void refuse_patch(MtLoader *L, const char *sha) {
    if (mt_strs_find(&L->refused, sha) < 0) mt_strs_add(&L->refused, sha);
    MtBuf b = {0};
    for (int i = 0; i < L->refused.n; i++) mt_buf_fmt(&b, "%s\n", L->refused.v[i]);
    char *p = mt_path_join(L->cache_dir, "patched/refused.txt");
    mt_write_file(p, b.p, b.n);
    free(p), free(b.p);
}

int mt_loader_apply_patches(MtLoader *L) {
    int total = 0;
    char *proot = mt_path_join(L->cache_dir, "patched");
    MtStrs keep = {0}, active = {0};
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK || !m->patch_root) continue;
        if (!L->be.game_file_size || !L->be.game_read) {
            mt_loader_note(L, m->m.id, "this game build cannot apply patches: its patched files are left out");
            continue;
        }
        MtStrs rels = {0}, shas = {0};
        mt_walk_files(m->patch_root, list_patch, &rels);
        /* its folder: the mod's id and a key of its patches and the game's build, so a new patch or game version starts afresh */
        MtSha256 ks;
        uint8_t key[32];
        char hex[65];
        mt_sha256_init(&ks);
        if (L->be.game_build_id) mt_sha256_update(&ks, L->be.game_build_id, strlen(L->be.game_build_id));
        for (int j = 0; j < rels.n; j++) {
            char *p = mt_path_join(m->patch_root, rels.v[j]);
            size_t n = 0;
            char *d = mt_read_file(p, &n);
            mt_sha256_hex(d ? d : "", d ? n : 0, hex);
            mt_strs_add(&shas, hex);
            mt_sha256_update(&ks, rels.v[j], strlen(rels.v[j]) + 1), mt_sha256_update(&ks, hex, 64);
            free(d), free(p);
        }
        mt_sha256_final(&ks, key);
        for (int j = 0; j < 6; j++) snprintf(hex + 2 * j, 3, "%02x", key[j]);
        char *name = mt_fmt("%s-%s", m->m.id, hex), *dir = mt_path_join(proot, name), *files = mt_path_join(dir, "files"),
             *done_p = mt_path_join(dir, "done.txt"), *done = mt_read_file(done_p, NULL);
        mt_strs_add(&keep, name), mt_strs_add(&active, m->m.id);
        MtBuf now = {0};
        int applied = 0, cached = 0, failed = 0, refused = 0;
        for (int j = 0; j < rels.n && !refused; j++) {
            char *rom = mt_strndup(rels.v[j], strlen(rels.v[j]) - 8), *line = mt_fmt("%s %s\n", shas.v[j], rom), *out = mt_path_join(files, rom);
            int64_t size;
            if (done && strstr(done, line) && mt_is_file(out)) {
                cached++, mt_buf_str(&now, line);
            } else if ((size = L->be.game_file_size(L->be.user, rom)) < 0) {
                mt_log(L, MT_LOG_WARN, "%s: it patches %s, which the game does not have", m->m.id, rom);
                failed++;
            } else {
                char *p = mt_path_join(m->patch_root, rels.v[j]), *why = NULL;
                size_t n = 0;
                char *d = mt_read_file(p, &n);
                game_src g = {L, rom};
                int was_refused = 0;
                if (size > (64 << 20)) mt_log(L, MT_LOG_INFO, "%s: patching %s (%llu MB, once)", m->m.id, rom, (unsigned long long)(size >> 20));
                if (d && !mt_patch_apply((const uint8_t *)d, n, (uint64_t)size, game_src_read, &g, out, &why, &was_refused)) {
                    applied++, mt_buf_str(&now, line);
                } else {
                    mt_log(L, MT_LOG_WARN, "%s: %s: %s", m->m.id, rom, why ? why : "cannot read the patch");
                    if (was_refused) refused++, refuse_patch(L, shas.v[j]);
                    else failed++;
                }
                free(why), free(d), free(p);
            }
            free(rom), free(line), free(out);
        }
        if (refused) {   /* the whole mod: its files and its other patches are left out too */
            mt_mod_message(m, MT_STATE_ERROR, "refused: a patch of it carries the game's own data, and a patch may carry only the change%s",
                           m->info.started ? " (its code runs until the game restarts)" : "");
            mt_remove_tree(dir);
        } else {
            mt_write_file(done_p, now.p ? now.p : "", now.n);
            if (applied + cached) {
                free(m->patched_root), m->patched_root = mt_path_abs(files);
                total += applied + cached;
                mt_log(L, MT_LOG_INFO, "%s: %d game file%s patched (%d already were)", m->m.id, applied + cached, applied + cached == 1 ? "" : "s", cached);
            }
            if (failed) {
                char *note = mt_fmt("%d of its patches did not apply (see the log)", failed);
                mt_loader_note(L, m->m.id, note);
                free(note);
            }
        }
        free(now.p), free(done), free(done_p), free(files), free(dir), free(name);
        mt_strs_free(&rels), mt_strs_free(&shas);
    }
    tidy_t t = {L, &keep, &active};
    if (mt_is_dir(proot)) mt_list_dir(proot, tidy_patched, &t);
    mt_strs_free(&keep), mt_strs_free(&active);
    free(proot);
    overlay_build(L);
    for (int i = 0; i < L->nmods; i++) mt_info_refresh(L->mods[i]);
    return total;
}

int mt_loader_point_count(MtLoader *L) {
    int n = 0;
    for (int i = 0; i < L->norder; i++)
        if (L->order[i]->info.state == MT_STATE_OK) n += L->order[i]->npts;
    return n;
}
int mt_loader_point(MtLoader *L, int i, uint64_t *addr, int *kind, const char **mod_id, const char **name) {
    for (int k = 0; k < L->norder; k++) {
        Mod *m = L->order[k];
        if (m->info.state != MT_STATE_OK) continue;
        if (i < m->npts) {
            if (addr) *addr = m->pts[i].addr;
            if (kind) *kind = m->pts[i].kind;
            if (mod_id) *mod_id = m->m.id;
            if (name) *name = m->pts[i].name;
            return MT_OK;
        }
        i -= m->npts;
    }
    return MT_ERR_UNKNOWN;
}
void mt_loader_note(MtLoader *L, const char *id, const char *text) {
    for (int i = 0; i < L->nmods; i++)
        if (!strcmp(L->mods[i]->m.id, id)) {
            Mod *m = L->mods[i];
            if (m->msg && *m->msg && !strstr(m->msg, text)) mt_mod_message(m, -1, "%s; %s", m->msg, text);
            else mt_mod_message(m, -1, "%s", text);
        }
}
