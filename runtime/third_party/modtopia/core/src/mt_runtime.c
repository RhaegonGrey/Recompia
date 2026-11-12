/* Modtopia core: running the mods. The MtHost table, loading code, hook dispatch, the event bus, the per-frame core events. MIT. */
#include "mt_loader.h"



/* One loader per process: MtHost calls without a mod (mem_read, call_guest, ...) reach it here */
static MtLoader *g_L;

struct MtHookCtx {
    MtLoader *L;
    Mod *mod;
    uint64_t site;
    int kind;
    void *host_ctx;
};

/* ---------------------------------------------------------------- the site table (open addressing; every possible site is in the plan) */
static uint64_t site_hash(uint64_t a) { a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33; return a; }
static Site *site_find(MtLoader *L, uint64_t addr) {
    if (!L->site_cap) return NULL;
    for (uint64_t i = site_hash(addr) & (uint64_t)(L->site_cap - 1);; i = (i + 1) & (uint64_t)(L->site_cap - 1)) {
        Site *s = &L->sites[i];
        if (s->addr == addr) return s;
        if (!s->addr) return NULL;
    }
}
/* The sites by the mods' declarations, with room for every declaring mod's handlers; addresses come from the running game's module bases */
void mt_sites_build(MtLoader *L) {
    if (!L->be.module_base) return;
    int n = 0;
    for (int i = 0; i < L->norder; i++) n += L->order[i]->npts;
    int cap = 16;
    while (cap < n * 2 + 2) cap *= 2;
    L->sites = mt_alloc(sizeof *L->sites * (size_t)cap);
    L->site_cap = cap;
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        for (int k = 0; k < m->npts; k++) {
            MtPoint *p = &m->pts[k];
            uint64_t base = L->be.module_base(L->be.user, p->module);
            p->addr = base ? base + p->offset : 0;
            if (!p->addr) continue;
            Site *s = site_find(L, p->addr);
            if (!s) {
                uint64_t j = site_hash(p->addr) & (uint64_t)(cap - 1);
                while (L->sites[j].addr) j = (j + 1) & (uint64_t)(cap - 1);
                s = &L->sites[j];
                s->addr = p->addr;
            }
            s->cap++;
        }
    }
    for (int i = 0; i < cap; i++)
        if (L->sites[i].addr) L->sites[i].h = mt_alloc(sizeof *L->sites[i].h * (size_t)(L->sites[i].cap ? L->sites[i].cap : 1));
}

int mt_hook_active(MtLoader *L, uint64_t site) {
    Site *s = L ? site_find(L, site) : NULL;
    return s && atomic_load_explicit(&s->n, memory_order_acquire) > 0;
}

int mt_hook_dispatch(MtLoader *L, uint64_t site, void *host_ctx) {
    Site *s = site_find(L, site);
    if (!s) return MT_CONTINUE;
    int n = atomic_load_explicit(&s->n, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        HookReg *h = &s->h[i];
        MtHookCtx c = {L, h->mod, site, h->kind, host_ctx};
        int r = h->fn(&c, h->user);
        if (r == MT_HANDLED && h->kind != MT_HOOK_OBSERVE) {
            if (!s->said) {
                s->said = 1;
                int others = 0;
                for (int j = i + 1; j < n; j++) others += s->h[j].kind != MT_HOOK_OBSERVE;
                mt_log(L, MT_LOG_INFO, "%s took %#llx%s", h->mod->m.id, (unsigned long long)site, others ? " (later mods there did not run)" : "");
            }
            return MT_HANDLED;
        }
    }
    return MT_CONTINUE;
}

/* ---------------------------------------------------------------- MtHost */
static MtLoader *loader_of(MtMod *m) { return m ? m->L : g_L; }
static void h_log(MtMod *m, int level, const char *line) {
    MtLoader *L = loader_of(m);
    char *s = mt_fmt("[%s] %s", m ? m->m.id : "?", line);
    if (L && L->be.log) L->be.log(L->be.user, level, s);
    else fprintf(stderr, "%s\n", s);
    free(s);
}
static const char *h_mod_id(MtMod *m) { return m->m.id; }
static const char *h_mod_version(MtMod *m) { return m->m.version; }
static const char *h_mod_dir(MtMod *m) { return m->dir; }

static int h_hook(MtMod *m, const char *point, MtHookFn fn, void *user) {
    MtLoader *L = m->L;
    if (!fn || !point) return MT_ERR;
    MtPoint *p = NULL;
    for (int i = 0; i < m->npts; i++)
        if (!strcmp(m->pts[i].name, point)) p = &m->pts[i];
    if (!p) {
        mt_log(L, MT_LOG_ERROR, "%s: no hook point '%s' is declared for game version %s in its mod.toml", m->m.id, point, L->be.game_version ? L->be.game_version : "?");
        return MT_ERR_UNKNOWN;
    }
    Site *s = p->addr ? site_find(L, p->addr) : NULL;
    if (!s) return MT_ERR_STATE;
    mt_lock(&L->lock);
    int n = s->n;
    if (n >= s->cap) {   /* the same point twice from one mod: the table has room for one handler a declaration */
        mt_unlock(&L->lock);
        mt_log(L, MT_LOG_ERROR, "%s: the hook point '%s' already has a handler", m->m.id, point);
        return MT_ERR_STATE;
    }
    s->h[n] = (HookReg){m, p->kind, fn, user};
    atomic_store_explicit(&s->n, n + 1, memory_order_release);
    mt_unlock(&L->lock);
    return MT_OK;
}
static uint64_t h_reg_get(MtHookCtx *c, int reg) {
    uint64_t v = 0;
    if (c && c->L->be.reg_get) c->L->be.reg_get(c->L->be.user, c->host_ctx, reg, &v);
    return v;
}
static int h_reg_set(MtHookCtx *c, int reg, uint64_t v) {
    if (!c || c->kind == MT_HOOK_OBSERVE || !c->L->be.reg_set) return MT_ERR_STATE;
    return c->L->be.reg_set(c->L->be.user, c->host_ctx, reg, v);
}
static int h_vreg_get(MtHookCtx *c, int v, void *out) {
    if (!c || !c->L->be.vreg_get) return MT_ERR_STATE;
    return c->L->be.vreg_get(c->L->be.user, c->host_ctx, v, out);
}
static int h_vreg_set(MtHookCtx *c, int v, const void *in) {
    if (!c || c->kind == MT_HOOK_OBSERVE || !c->L->be.vreg_set) return MT_ERR_STATE;
    return c->L->be.vreg_set(c->L->be.user, c->host_ctx, v, in);
}
static uint64_t h_hook_address(MtHookCtx *c) { return c ? c->site : 0; }
static int h_hook_kind(MtHookCtx *c) { return c ? c->kind : 0; }
static int h_call_original(MtHookCtx *c) {
    if (!c || c->kind != MT_HOOK_REPLACE || !c->L->be.call_original) return MT_ERR_STATE;
    return c->L->be.call_original(c->L->be.user, c->host_ctx, c->site);
}
static int h_mem_read(uint64_t a, void *out, size_t n) {
    MtLoader *L = g_L;
    return L && L->be.mem_read ? L->be.mem_read(L->be.user, a, out, n) : MT_ERR_STATE;
}
static int h_mem_write(uint64_t a, const void *in, size_t n) {
    MtLoader *L = g_L;
    return L && L->be.mem_write ? L->be.mem_write(L->be.user, a, in, n) : MT_ERR_STATE;
}
static uint64_t h_module_base(const char *module) {
    MtLoader *L = g_L;
    return L && L->be.module_base ? L->be.module_base(L->be.user, module) : 0;
}
static uint64_t h_resolve(MtMod *m, const char *name) {
    MtLoader *L = loader_of(m);
    char module[16];
    uint64_t off;
    if (!L || !mt_resolve_name(L, name, NULL, module, &off)) return 0;
    uint64_t base = h_module_base(module);
    return base ? base + off : 0;
}
static int h_call_guest(uint64_t addr, MtCall *regs) {
    MtLoader *L = g_L;
    return L && L->be.call_guest ? L->be.call_guest(L->be.user, NULL, addr, regs) : MT_ERR_STATE;
}

/* ---------------------------------------------------------------- events */
static EventSlot *event_slot(MtLoader *L, const char *name, int make) {
    for (int i = 0; i < L->nevents; i++)
        if (!strcmp(L->events[i].name, name)) return &L->events[i];
    if (!make) return NULL;
    if (L->nevents == L->events_cap) {
        /* slots never move once handed out: a new table, the old one leaked (posting threads may be reading it) */
        int cap = L->events_cap ? L->events_cap * 2 : 16;
        EventSlot *e = mt_alloc(sizeof *e * (size_t)cap);
        memcpy(e, L->events, sizeof *e * (size_t)L->nevents);
        L->events = e;
        L->events_cap = cap;
    }
    EventSlot *s = &L->events[L->nevents];
    s->name = mt_strdup(name);
    s->list = NULL;
    L->nevents++;
    return s;
}
static int h_subscribe(MtMod *m, const char *event, int prio, MtEventFn fn, void *user) {
    MtLoader *L = m->L;
    if (!event || !fn) return MT_ERR;
    mt_lock(&L->lock);
    EventSlot *s = event_slot(L, event, 1);
    SubList *old = s->list;
    int n = old ? old->n : 0;
    SubList *nl = mt_alloc(sizeof *nl + sizeof(Sub) * (size_t)(n + 1));
    Sub add = {m, prio, L->sub_seq++, fn, user};
    int j = 0, placed = 0;
    for (int i = 0; i < n; i++) {   /* highest priority first; equal priorities in the order they subscribed (the load order at init) */
        if (!placed && add.prio > old->s[i].prio) nl->s[j++] = add, placed = 1;
        nl->s[j++] = old->s[i];
    }
    if (!placed) nl->s[j++] = add;
    nl->n = j;
    s->list = nl;   /* the old list stays: a post on another thread may be walking it */
    mt_unlock(&L->lock);
    return MT_OK;
}
static int post(MtLoader *L, Mod *from, const char *event, const void *data, uint32_t size, int cancellable) {
    EventSlot *s = NULL;
    for (int i = 0; i < L->nevents; i++)
        if (!strcmp(L->events[i].name, event)) s = &L->events[i];
    SubList *l = s ? s->list : NULL;
    if (!l) return 0;
    MtEvent ev = {event, data, size, (uint32_t)!!cancellable, L->frame};
    for (int i = 0; i < l->n; i++) {
        int r = l->s[i].fn(l->s[i].mod, &ev, l->s[i].user);
        if (cancellable && r == MT_CANCEL) {
            mt_log(L, MT_LOG_DEBUG, "%s cancelled %s%s%s", l->s[i].mod->m.id, event, from ? " from " : "", from ? from->m.id : "");
            return 1;
        }
    }
    return 0;
}
static int h_post(MtMod *m, const char *event, const void *data, uint32_t size, int cancellable) {
    MtLoader *L = m->L;
    size_t k = strlen(m->m.id);
    if (strncmp(event, m->m.id, k) || event[k] != ':') {
        mt_log(L, MT_LOG_ERROR, "%s: a mod's own events are named \"%s:<name>\" (not \"%s\")", m->m.id, m->m.id, event);
        return MT_ERR;
    }
    return post(L, m, event, data, size, cancellable);
}

/* ---------------------------------------------------------------- config */
static int setting_index(Mod *m, const char *key) {
    for (int i = 0; m->cfg && i < m->m.nsettings; i++)
        if (!strcmp(m->m.settings[i].info.key, key)) return i;
    return -1;
}
static int h_config_bool(MtMod *m, const char *key) {
    int i = setting_index(m, key);
    return i >= 0 && m->cfg[i].num != 0;
}
static double h_config_number(MtMod *m, const char *key) {
    int i = setting_index(m, key);
    return i >= 0 ? m->cfg[i].num : 0;
}
static const char *h_config_string(MtMod *m, const char *key) {
    int i = setting_index(m, key);
    if (i < 0) return "";
    MtSetting *s = &m->m.settings[i];
    return s->info.type == MT_SET_CHOICE ? s->choices.v[(int)m->cfg[i].num] : m->cfg[i].text;
}
static int h_on_config_changed(MtMod *m, MtConfigFn fn, void *user) {
    m->cfg_fn = fn;
    m->cfg_user = user;
    return MT_OK;
}
void mt_config_notify(Mod *m, const char *key) {
    int i = setting_index(m, key);
    if (i >= 0 && m->m.settings[i].info.restart) return;   /* it changes at the next start */
    if (m->cfg_fn && m->info.started) m->cfg_fn(m, key, m->cfg_user);
}
static uint64_t h_frame(void) { return g_L ? g_L->frame : 0; }

/* ---------------------------------------------------------------- sound (API 0.5): each mod that plays gets one of the host's voices */
static int voice_of(MtMod *m) {
    MtLoader *L = m->L;
    if (!m->voice) {
        mt_lock(&L->lock);
        if (!m->voice && L->voices < MT_AUDIO_VOICES) m->voice = ++L->voices;
        mt_unlock(&L->lock);
    }
    return m->voice - 1;
}
static int h_audio_write(MtMod *m, const int16_t *stereo, int frames) {
    MtLoader *L = m->L;
    int v;
    if (!L->be.audio_write) return MT_ERR_VERSION;
    if (!stereo || frames < 0) return MT_ERR;
    if ((v = voice_of(m)) < 0) return MT_ERR_STATE;   /* every voice is taken */
    return L->be.audio_write(L->be.user, v, stereo, frames);
}
static int h_audio_queued(MtMod *m) {
    MtLoader *L = m->L;
    if (!L->be.audio_queued) return MT_ERR_VERSION;
    return m->voice ? L->be.audio_queued(L->be.user, m->voice - 1) : 0;
}
static int h_audio_clear(MtMod *m) {
    MtLoader *L = m->L;
    if (!L->be.audio_clear) return MT_ERR_VERSION;
    return m->voice ? L->be.audio_clear(L->be.user, m->voice - 1) : MT_OK;
}

/* ---------------------------------------------------------------- the player's side (API 0.2): through the host's backend */
static int h_notify(MtMod *m, const char *text, double seconds) {
    MtLoader *L = m->L;
    if (!L->be.notify) return MT_ERR_VERSION;
    return L->be.notify(L->be.user, m->m.id, text ? text : "", seconds > 0 ? seconds : 3);
}
static int h_notify_image(MtMod *m, const char *text, double seconds, const char *png_path) {
    MtLoader *L = m->L;
    size_t n = 0;
    int w = 0, h = 0, rc = MT_ERR;
    if (!L->be.notify_image || !png_path) return h_notify(m, text, seconds);
    char *file = mt_read_file(png_path, &n);
    uint8_t *px = file ? mt_png_rgba((const uint8_t *)file, n, &w, &h) : NULL, *fit = px ? malloc((size_t)MT_NOTIFY_IMAGE * MT_NOTIFY_IMAGE * 4) : NULL;
    if (fit) {
        mt_png_fit(px, w, h, fit, MT_NOTIFY_IMAGE);
        rc = L->be.notify_image(L->be.user, m->m.id, text ? text : "", seconds > 0 ? seconds : 3, fit);
    }
    free(file), free(px), free(fit);
    return fit ? rc : h_notify(m, text, seconds);   /* no picture after all: the text alone */
}
static char *parent_dir(const char *p) {
    char *a = mt_path_abs(p);
    char *s = strrchr(a, '/'), *b = strrchr(a, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
    return a;
}
static int is_absolute(const char *p) { return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'); }
static int h_screenshot(MtMod *m, const char *path, char *out, size_t out_n) {
    MtLoader *L = m->L;
    if (!L->be.screenshot) return MT_ERR_VERSION;
    char *full;
    if (path && is_absolute(path)) {
        full = mt_strdup(path);
    } else {   /* the game's screenshots folder, beside its mods folder */
        char *top = parent_dir(L->mods_dir), *shots = mt_path_join(top, "screenshots");
        full = mt_path_join(shots, path && *path ? path : "shot.png");
        free(top), free(shots);
    }
    char *dir = parent_dir(full);
    mt_mkdirs(dir);
    int r = L->be.screenshot(L->be.user, full);
    if (out && out_n) snprintf(out, out_n, "%s", full);
    free(dir), free(full);
    return r;
}
static const char *h_save_dir(MtMod *m) { return m->L->be.save_dir ? m->L->be.save_dir(m->L->be.user) : NULL; }
static const char *h_data_dir(MtMod *m) {
    if (!m->data_dir) {
        m->data_dir = mt_path_join(m->L->config_dir, m->m.id);
        mt_mkdirs(m->data_dir);
    }
    return m->data_dir;
}
static int h_pad_read(MtMod *m, int p, MtPad *out) {
    MtLoader *L = m->L;
    if (!L->be.pad_read) return MT_ERR_VERSION;
    if (p < 0 || p >= MT_PLAYERS || !out) return MT_ERR_UNKNOWN;
    return L->be.pad_read(L->be.user, p, out);
}
static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
/* every mod's held input for player p, combined: buttons ORed, sticks added */
static int h_pad_hold(MtMod *m, int p, const MtPad *add) {
    MtLoader *L = m->L;
    if (!L->be.pad_hold) return MT_ERR_VERSION;
    if (p < 0 || p >= MT_PLAYERS) return MT_ERR_UNKNOWN;
    mt_lock(&L->lock);
    m->holding[p] = add != NULL;
    if (add) m->hold[p] = *add;
    int any = 0, lx = 0, ly = 0, rx = 0, ry = 0;
    MtPad sum = {0};
    for (int i = 0; i < L->norder; i++) {
        Mod *o = L->order[i];
        if (!o->holding[p]) continue;
        any = 1, sum.buttons |= o->hold[p].buttons;
        lx += o->hold[p].lx, ly += o->hold[p].ly, rx += o->hold[p].rx, ry += o->hold[p].ry;
    }
    sum.lx = clamp16(lx), sum.ly = clamp16(ly), sum.rx = clamp16(rx), sum.ry = clamp16(ry);
    int r = L->be.pad_hold(L->be.user, p, any ? &sum : NULL);
    mt_unlock(&L->lock);
    return r;
}
static int h_pad_block(MtMod *m, int p, const MtPad *mask) {
    MtLoader *L = m->L;
    if (!L->be.pad_block) return MT_ERR_VERSION;
    if (p < 0 || p >= MT_PLAYERS) return MT_ERR_UNKNOWN;
    mt_lock(&L->lock);
    m->blocking[p] = mask != NULL;
    if (mask) m->block[p] = *mask;
    int any = 0;
    MtPad sum = {0};
    for (int i = 0; i < L->norder; i++) {
        Mod *o = L->order[i];
        if (!o->blocking[p]) continue;
        any = 1, sum.buttons |= o->block[p].buttons;
        sum.lx |= o->block[p].lx || o->block[p].ly, sum.rx |= o->block[p].rx || o->block[p].ry;
    }
    sum.ly = sum.lx, sum.ry = sum.rx;
    int r = L->be.pad_block(L->be.user, p, any ? &sum : NULL);
    mt_unlock(&L->lock);
    return r;
}
static int h_ui_visible(MtMod *m, int visible) {
    MtLoader *L = m->L;
    if (!L->be.ui_visible) return MT_ERR_VERSION;
    mt_lock(&L->lock);
    m->hides_ui = !visible;
    int hidden = 0;
    for (int i = 0; i < L->norder; i++) hidden |= L->order[i]->hides_ui;
    int r = L->be.ui_visible(L->be.user, !hidden);
    mt_unlock(&L->lock);
    return r;
}
static int h_key_down(MtMod *m, const char *key) { return m->L->be.key_down && key && *key ? m->L->be.key_down(m->L->be.user, key) : 0; }
/* the final picture's matrix: the last mod loaded that set one */
static int h_color_matrix(MtMod *m, const float *m12) {
    MtLoader *L = m->L;
    if (!L->be.color_matrix) return MT_ERR_VERSION;
    mt_lock(&L->lock);
    m->has_cmat = m12 != NULL;
    if (m12) memcpy(m->cmat, m12, sizeof m->cmat);
    const float *eff = NULL;
    for (int i = L->norder - 1; i >= 0 && !eff; i--)
        if (L->order[i]->has_cmat) eff = L->order[i]->cmat;
    int r = L->be.color_matrix(L->be.user, eff);
    mt_unlock(&L->lock);
    return r;
}

void mt_host_init(MtLoader *L) {
    MtHost *h = &L->host;
    h->size = sizeof *h;
    h->api_version = MT_API_VERSION;
    h->log = h_log;
    h->mod_id = h_mod_id;
    h->mod_version = h_mod_version;
    h->mod_dir = h_mod_dir;
    h->hook = h_hook;
    h->reg_get = h_reg_get;
    h->reg_set = h_reg_set;
    h->vreg_get = h_vreg_get;
    h->vreg_set = h_vreg_set;
    h->hook_address = h_hook_address;
    h->hook_kind = h_hook_kind;
    h->call_original = h_call_original;
    h->mem_read = h_mem_read;
    h->mem_write = h_mem_write;
    h->module_base = h_module_base;
    h->resolve = h_resolve;
    h->call_guest = h_call_guest;
    h->subscribe = h_subscribe;
    h->post = h_post;
    h->config_bool = h_config_bool;
    h->config_number = h_config_number;
    h->config_string = h_config_string;
    h->on_config_changed = h_on_config_changed;
    h->frame = h_frame;
    h->notify = h_notify;
    h->screenshot = h_screenshot;
    h->save_dir = h_save_dir;
    h->data_dir = h_data_dir;
    h->pad_read = h_pad_read;
    h->pad_hold = h_pad_hold;
    h->key_down = h_key_down;
    h->color_matrix = h_color_matrix;
    h->pad_block = h_pad_block;
    h->ui_visible = h_ui_visible;
    h->net_request = mt_net_request;
    h->audio_write = h_audio_write;
    h->audio_queued = h_audio_queued;
    h->audio_clear = h_audio_clear;
    h->notify_image = h_notify_image;
}

/* ---------------------------------------------------------------- start: check the points against the game, load the code */
static int check_points(MtLoader *L, Mod *m) {
    for (int k = 0; k < m->npts; k++) {
        MtPoint *p = &m->pts[k];
        if (!p->addr) {
            mt_mod_message(m, MT_STATE_ERROR, "the game has no module %s for the hook point '%s'", p->module, p->name);
            return 0;
        }
        uint32_t w = 0;
        if (L->be.mem_read && L->be.mem_read(L->be.user, p->addr, &w, 4) == MT_OK && w != p->word) {
            mt_mod_message(m, MT_STATE_ERROR, "the game's instruction at %s+%#llx is %08x, not %08x as '%s' expects: it does not fit this game build",
                           p->module, (unsigned long long)p->offset, w, p->word, p->name);
            return 0;
        }
        if (L->be.hook_built_in && !L->be.hook_built_in(L->be.user, p->addr, p->kind)) {
            mt_mod_message(m, MT_STATE_OFF, "its hook points are not built into the game yet: start the game from its launcher (which rebuilds it) or "
                                            "use Update my game in the builder");
            return 0;
        }
    }
    return 1;
}

int mt_loader_start(MtLoader *L) {
    g_L = L;
    L->host.host_name = L->be.host_name;
    L->host.game_version = L->be.game_version ? L->be.game_version : "";
    L->host.game_build_id = L->be.game_build_id ? L->be.game_build_id : "";
    L->host.platform = L->be.platform;
    if (L->mappings) {   /* the area for AreaEntered: a global pointer and a field of what it points at ([core] area in the mappings) */
        char module[16];
        uint64_t off;
        MtToml *area = mt_toml_get(mt_toml_get(L->mappings, "core"), "area");
        const char *gname = area && area->type == MT_T_TABLE ? mt_toml_str(area, "global", NULL) : NULL;
        const char *fname = area && area->type == MT_T_TABLE ? mt_toml_str(area, "field", NULL) : NULL;
        const char *rname = area && area->type == MT_T_TABLE ? mt_toml_str(area, "ready", NULL) : NULL;
        MtToml *f = fname ? mt_toml_get(mt_toml_get(L->mappings, "fields"), fname) : NULL;
        MtToml *r = rname ? mt_toml_get(mt_toml_get(L->mappings, "fields"), rname) : NULL;
        if (gname && mt_resolve_name(L, gname, NULL, module, &off) && f && f->type == MT_T_TABLE && L->be.module_base) {
            uint64_t base = L->be.module_base(L->be.user, module);
            L->area_global = base ? base + off : 0;
            L->area_off = (uint32_t)mt_toml_int(f, "offset", 0);
            L->area_size = (uint32_t)mt_toml_int(f, "size", 1);
            if (L->area_size != 1 && L->area_size != 2 && L->area_size != 4) L->area_size = 1;
            if (r && r->type == MT_T_TABLE) {
                L->ready_off = (uint32_t)mt_toml_int(r, "offset", 0);
                L->ready_size = (uint32_t)mt_toml_int(r, "size", 4);
                if (L->ready_size != 1 && L->ready_size != 2 && L->ready_size != 4 && L->ready_size != 8) L->ready_size = 4;
            }
        }
    }
    int started = 0;
    for (int i = 0; i < L->norder; i++) {
        Mod *m = L->order[i];
        if (m->info.state != MT_STATE_OK) continue;
        if (m->npts && !check_points(L, m)) { mt_log(L, MT_LOG_WARN, "%s is off: %s", m->m.id, m->msg); continue; }
        if (!m->lib_path) continue;
        if (L->no_code) { mt_mod_message(m, MT_STATE_OFF, "code mods are off this time"); continue; }
        if (m->info.flags & MT_MOD_NEW_CODE) mt_log(L, MT_LOG_WARN, "%s %s has new or changed code (sha256 %.16s...)", m->m.id, m->m.version, m->sha);
        if (m->m.network.n) mt_log(L, MT_LOG_INFO, "%s may reach: %s%s", m->m.id, m->info.network, L->be.net_request ? "" : " (this host has no network for mods)");
        char err[300] = "";
        m->lib = mt_lib_open(m->lib_path, err, sizeof err);
        if (!m->lib) { mt_mod_message(m, MT_STATE_ERROR, "its code cannot be loaded: %s", err); mt_log(L, MT_LOG_ERROR, "%s: %s", m->m.id, m->msg); continue; }
        const uint32_t *api = mt_lib_sym(m->lib, "modtopia_api_version");
        if (!api || (*api >> 16) != MT_API_VERSION_MAJOR || (*api & 0xffff) > MT_API_VERSION_MINOR) {
            mt_mod_message(m, MT_STATE_ERROR, "built for Modtopia SDK %u.%u; this loader has %u.%u", api ? *api >> 16 : 0, api ? *api & 0xffff : 0,
                           MT_API_VERSION_MAJOR, MT_API_VERSION_MINOR);
            mt_log(L, MT_LOG_ERROR, "%s: %s", m->m.id, m->msg);
            continue;
        }
        MtModInitFn init = (MtModInitFn)mt_lib_sym(m->lib, m->m.entry);
        if (!init) { mt_mod_message(m, MT_STATE_ERROR, "its code has no entry point %s", m->m.entry); mt_log(L, MT_LOG_ERROR, "%s: %s", m->m.id, m->msg); continue; }
        m->info.started = 1;   /* before the call: a config callback it registers may fire */
        int rc = init(&L->host, m);
        if (rc != MT_OK) {
            m->info.started = 0;
            mt_mod_message(m, MT_STATE_ERROR, "its start failed (code %d)", rc);
            mt_log(L, MT_LOG_ERROR, "%s: %s", m->m.id, m->msg);
            continue;
        }
        started++;
        mt_log(L, MT_LOG_INFO, "%s %s started", m->m.id, m->m.version);
    }
    L->started = 1;
    return started;
}

/* ---------------------------------------------------------------- per frame */
void mt_loader_frame(MtLoader *L) {
    uint64_t f = ++L->frame;
    MtFrameData fd = {f};
    mt_net_frame(L);
    post(L, NULL, "FrameStart", &fd, sizeof fd, 0);
    if (!L->area_global || !L->be.mem_read || (f & 7)) return;   /* the area: read every 8 frames */
    uint64_t obj = 0;
    if (L->be.mem_read(L->be.user, L->area_global, &obj, 8) != MT_OK || !obj) return;
    if (L->ready_size) {   /* the world is up (a save loaded) only while the ready field is not zero: the title has an area too */
        uint64_t rv = 0;
        if (L->be.mem_read(L->be.user, obj + L->ready_off, &rv, L->ready_size) != MT_OK) return;
        if (!rv) { L->world = 0; return; }
    }
    uint32_t v = 0;
    if (L->be.mem_read(L->be.user, obj + L->area_off, &v, L->area_size) != MT_OK) return;
    int32_t area = (int32_t)(L->area_size == 1 ? (uint8_t)v : L->area_size == 2 ? (uint16_t)v : v);
    if (!area) return;   /* none yet: a loading screen */
    int entered = !L->world;   /* into the world from the title (or the first time): the save was loaded */
    if (!entered && L->area_valid && area == L->area) return;
    int32_t prev = L->area_valid ? L->area : -1;
    L->area = area;
    L->area_valid = 1;
    L->world = 1;
    if (entered) {   /* each time the world comes up from the title: the save has been loaded */
        MtSaveData sd = {++L->saves_loaded};
        mt_log(L, MT_LOG_INFO, "SaveLoaded (frame %llu)", (unsigned long long)f);
        post(L, NULL, "SaveLoaded", &sd, sizeof sd, 0);
    }
    MtAreaData ad = {area, prev, mt_mapping_area_name(L, area)};
    mt_log(L, MT_LOG_INFO, "AreaEntered %d%s%s%s (frame %llu)", area, ad.name ? " (" : "", ad.name ? ad.name : "", ad.name ? ")" : "", (unsigned long long)f);
    post(L, NULL, "AreaEntered", &ad, sizeof ad, 0);
}
void mt_loader_save_written(MtLoader *L) {
    MtSaveData sd = {++L->saves_written};
    post(L, NULL, "SaveWritten", &sd, sizeof sd, 0);
}
