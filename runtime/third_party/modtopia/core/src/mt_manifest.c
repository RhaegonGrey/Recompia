/* Modtopia core: reading and checking mod.toml. MIT. */
#include "mt_manifest.h"

#include <ctype.h>

int mt_id_valid(const char *id) {   /* lowercase letters, digits, '_' and '-', starting with a letter; 2..64 characters */
    size_t n = strlen(id);
    if (n < 2 || n > 64 || !islower((unsigned char)id[0])) return 0;
    for (const char *c = id; *c; c++)
        if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_' || *c == '-')) return 0;
    return 1;
}

static int bad(char *err, size_t errn, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errn, fmt, ap);
    va_end(ap);
    return -1;
}

static int str_list(const MtToml *t, const char *key, MtStrs *out, char *err, size_t errn) {
    MtToml *v = mt_toml_get(t, key);
    if (!v) return 0;
    if (v->type == MT_T_STRING) { mt_strs_add(out, v->s); return 0; }
    if (v->type != MT_T_ARRAY) return bad(err, errn, "'%s' must be a list of strings", key);
    for (int i = 0; i < v->n; i++) {
        if (v->items[i]->type != MT_T_STRING) return bad(err, errn, "'%s' must be a list of strings", key);
        mt_strs_add(out, v->items[i]->s);
    }
    return 0;
}

static int kind_of(const char *s) {
    if (!s) return 0;
    if (!strcmp(s, "observe")) return MT_HOOK_OBSERVE;
    if (!strcmp(s, "take_over") || !strcmp(s, "take-over") || !strcmp(s, "redirect")) return MT_HOOK_TAKE_OVER;
    if (!strcmp(s, "replace")) return MT_HOOK_REPLACE;
    return 0;
}

static int parse_setting(const MtToml *t, MtSetting *s, char *err, size_t errn) {
    const char *key = mt_toml_str(t, "key", NULL), *type = mt_toml_str(t, "type", NULL);
    if (!key || !*key) return bad(err, errn, "a [[settings]] entry has no key");
    for (const char *c = key; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '-' || *c == '.')) return bad(err, errn, "the setting key '%s' may only have letters, digits, '_', '-' and '.'", key);
    if (!type) return bad(err, errn, "the setting '%s' has no type", key);
    MtSettingInfo *i = &s->info;
    i->key = mt_strdup(key);
    i->label = mt_strdup(mt_toml_str(t, "label", key));
    i->description = mt_strdup(mt_toml_str(t, "description", ""));
    i->restart = mt_toml_bool(t, "restart", 0);
    MtToml *def = mt_toml_get(t, "default");
    if (!strcmp(type, "bool")) {
        i->type = MT_SET_BOOL;
        if (def && def->type != MT_T_BOOL) return bad(err, errn, "the setting '%s': default must be true or false", key);
        s->def_num = def ? def->b : 0;
    } else if (!strcmp(type, "number") || !strcmp(type, "int") || !strcmp(type, "float")) {
        i->type = MT_SET_NUMBER;
        i->min = mt_toml_num(t, "min", -1e300);
        i->max = mt_toml_num(t, "max", 1e300);
        i->step = mt_toml_num(t, "step", !strcmp(type, "int") ? 1 : 0);
        if (!mt_toml_get(t, "min") || !mt_toml_get(t, "max")) return bad(err, errn, "the setting '%s': a number needs min and max", key);
        if (i->min > i->max) return bad(err, errn, "the setting '%s': min is above max", key);
        if (def && def->type != MT_T_INT && def->type != MT_T_FLOAT) return bad(err, errn, "the setting '%s': default must be a number", key);
        s->def_num = def ? (def->type == MT_T_INT ? (double)def->i : def->f) : i->min;
        if (s->def_num < i->min || s->def_num > i->max) return bad(err, errn, "the setting '%s': default is outside min..max", key);
    } else if (!strcmp(type, "choice")) {
        i->type = MT_SET_CHOICE;
        if (str_list(t, "choices", &s->choices, err, errn)) return -1;
        if (!s->choices.n) return bad(err, errn, "the setting '%s': a choice needs choices", key);
        i->nchoices = s->choices.n;
        i->choices = (const char *const *)s->choices.v;
        int at = 0;
        if (def) {
            if (def->type != MT_T_STRING || (at = mt_strs_find(&s->choices, def->s)) < 0) return bad(err, errn, "the setting '%s': default must be one of its choices", key);
        }
        s->def_num = at;
    } else if (!strcmp(type, "text") || !strcmp(type, "key")) {
        i->type = !strcmp(type, "text") ? MT_SET_TEXT : MT_SET_KEY;
        if (def && def->type != MT_T_STRING) return bad(err, errn, "the setting '%s': default must be a string", key);
        s->def_text = mt_strdup(def ? def->s : "");
    } else {
        return bad(err, errn, "the setting '%s' has an unknown type '%s' (bool, number, choice, text, key)", key, type);
    }
    return 0;
}

static int parse_hooks(const MtToml *hooks, MtManifest *m, char *err, size_t errn) {
    if (hooks->type != MT_T_TABLE) return bad(err, errn, "'hooks' must be tables per game version: [[hooks.\"2.0.1\"]]");
    m->hooks = mt_alloc(sizeof *m->hooks * (size_t)(hooks->n ? hooks->n : 1));
    for (int g = 0; g < hooks->n; g++) {
        MtToml *list = hooks->items[g];
        if (list->type != MT_T_ARRAY) return bad(err, errn, "hooks.\"%s\" must be a list of [[hooks.\"%s\"]] tables", hooks->keys[g], hooks->keys[g]);
        MtHookSet *set = &m->hooks[m->nhooks++];
        set->game = mt_strdup(hooks->keys[g]);
        set->h = mt_alloc(sizeof *set->h * (size_t)(list->n ? list->n : 1));
        for (int i = 0; i < list->n; i++) {
            MtToml *t = list->items[i];
            if (t->type != MT_T_TABLE) return bad(err, errn, "hooks.\"%s\": each entry must be a table", set->game);
            MtHookDecl *h = &set->h[set->n++];
            const char *name = mt_toml_str(t, "name", NULL), *target = mt_toml_str(t, "target", NULL), *kind = mt_toml_str(t, "kind", NULL);
            if (!name || !*name) return bad(err, errn, "a hook point for %s has no name", set->game);
            if (!target || !*target) return bad(err, errn, "the hook point '%s' (%s) has no target", name, set->game);
            for (int j = 0; j < set->n - 1; j++)
                if (!strcmp(set->h[j].name, name)) return bad(err, errn, "the hook point '%s' is declared twice for %s", name, set->game);
            h->name = mt_strdup(name);
            h->target = mt_strdup(target);
            h->offset = mt_toml_int(t, "offset", 0);
            MtToml *w = mt_toml_get(t, "word");
            if (w && w->type != MT_T_INT) return bad(err, errn, "the hook point '%s': word must be a number (0x...)", name);
            h->word = w ? (int64_t)(uint32_t)w->i : -1;
            h->kind = kind_of(kind);
            if (!h->kind) return bad(err, errn, "the hook point '%s': kind must be observe, take_over or replace", name);
            int by_offset = strchr(target, '+') != NULL;
            if (by_offset && h->word < 0) return bad(err, errn, "the hook point '%s' targets %s: it needs the expected instruction word (word = 0x...)", name, target);
            if (h->offset && h->word < 0) return bad(err, errn, "the hook point '%s' is inside a function: it needs the expected instruction word", name);
            if (h->kind == MT_HOOK_REPLACE && h->offset) return bad(err, errn, "the hook point '%s': replace is at a function's entry (no offset)", name);
            if (h->offset & 3) return bad(err, errn, "the hook point '%s': the offset is not on an instruction", name);
        }
    }
    return 0;
}

int mt_manifest_parse(const char *text, MtManifest *m, char *err, size_t errn) {
    memset(m, 0, sizeof *m);
    char terr[300];
    MtToml *t = mt_toml_parse(text, terr, sizeof terr);
    if (!t) return bad(err, errn, "mod.toml: %s", terr);
    int rc = -1;
    const char *id = mt_toml_str(t, "id", NULL), *version = mt_toml_str(t, "version", NULL);
    MtVer v;
    if (!id) { bad(err, errn, "mod.toml has no id"); goto out; }
    if (!mt_id_valid(id)) { bad(err, errn, "the id '%s' must be 2-64 lowercase letters, digits, '_' or '-', starting with a letter", id); goto out; }
    if (!version || mt_ver_parse(version, &v, 0)) { bad(err, errn, "the version must be a semantic version like 1.0.0 (it is '%s')", version ? version : ""); goto out; }
    m->id = mt_strdup(id);
    m->version = mt_strdup(version);
    m->name = mt_strdup(mt_toml_str(t, "name", id));
    m->description = mt_strdup(mt_toml_str(t, "description", ""));
    m->license = mt_strdup(mt_toml_str(t, "license", ""));
    m->homepage = mt_strdup(mt_toml_str(t, "homepage", ""));
    m->icon = mt_strdup(mt_toml_str(t, "icon", ""));
    m->loader = mt_strdup(mt_toml_str(t, "loader", ""));
    m->entry = mt_strdup(mt_toml_str(t, "entry", "mod_init"));
    m->data = mt_strdup(mt_toml_str(t, "data", "assets"));
    if (!mt_zip_safe_name(m->data)) { bad(err, errn, "'data' must be a folder inside the package"); goto out; }
    if (m->loader[0] && mt_version_satisfies("0.0.0", m->loader) < 0) { bad(err, errn, "the loader range '%s' cannot be read", m->loader); goto out; }
    MtStrs authors = {0};
    if (str_list(t, "authors", &authors, err, errn)) { mt_strs_free(&authors); goto out; }
    MtBuf ab = {0};
    for (int i = 0; i < authors.n; i++) mt_buf_fmt(&ab, "%s%s", i ? ", " : "", authors.v[i]);
    m->authors = mt_buf_take(&ab);
    mt_strs_free(&authors);
    if (str_list(t, "game", &m->games, err, errn) || str_list(t, "platforms", &m->platforms, err, errn)) goto out;
    if (str_list(t, "network", &m->network, err, errn)) goto out;
    for (int i = 0; i < m->network.n; i++)
        if (!mt_net_host_valid(m->network.v[i])) { bad(err, errn, "'network' names hosts only, like \"stats.example.org\": '%s' is not one", m->network.v[i]); goto out; }
    const char *affects = mt_toml_str(t, "affects", "gameplay");
    if (strcmp(affects, "gameplay") && strcmp(affects, "visual")) { bad(err, errn, "affects must be \"gameplay\" or \"visual\""); goto out; }
    m->gameplay = !strcmp(affects, "gameplay");

    MtToml *deps = mt_toml_get(t, "dependencies");
    if (deps) {
        if (deps->type != MT_T_ARRAY) { bad(err, errn, "dependencies must be [[dependencies]] tables"); goto out; }
        m->deps = mt_alloc(sizeof *m->deps * (size_t)(deps->n ? deps->n : 1));
        for (int i = 0; i < deps->n; i++) {
            MtToml *d = deps->items[i];
            const char *did = d->type == MT_T_TABLE ? mt_toml_str(d, "id", NULL) : NULL;
            if (!did || !mt_id_valid(did)) { bad(err, errn, "a dependency has no valid id"); goto out; }
            if (!strcmp(did, m->id)) { bad(err, errn, "the mod depends on itself"); goto out; }
            MtDep *x = &m->deps[m->ndeps++];
            x->id = mt_strdup(did);
            x->range = mt_strdup(mt_toml_str(d, "version", ""));
            if (x->range[0] && mt_version_satisfies("0.0.0", x->range) < 0) { bad(err, errn, "the dependency %s: the version range '%s' cannot be read", did, x->range); goto out; }
            x->required = mt_toml_bool(d, "required", 1);
            const char *o = mt_toml_str(d, "order", "after");
            x->order = !strcmp(o, "after") ? 1 : !strcmp(o, "before") ? -1 : !strcmp(o, "none") ? 0 : 2;
            if (x->order == 2) { bad(err, errn, "the dependency %s: order must be before, after or none", did); goto out; }
        }
    }
    MtToml *sets = mt_toml_get(t, "settings");
    if (sets) {
        if (sets->type != MT_T_ARRAY) { bad(err, errn, "settings must be [[settings]] tables"); goto out; }
        m->settings = mt_alloc(sizeof *m->settings * (size_t)(sets->n ? sets->n : 1));
        for (int i = 0; i < sets->n; i++) {
            if (sets->items[i]->type != MT_T_TABLE) { bad(err, errn, "settings must be [[settings]] tables"); goto out; }
            if (parse_setting(sets->items[i], &m->settings[m->nsettings++], err, errn)) goto out;
            for (int j = 0; j < m->nsettings - 1; j++)
                if (!strcmp(m->settings[j].info.key, m->settings[i].info.key)) { bad(err, errn, "the setting '%s' is declared twice", m->settings[i].info.key); goto out; }
        }
    }
    MtToml *hooks = mt_toml_get(t, "hooks");
    if (hooks && parse_hooks(hooks, m, err, errn)) goto out;
    rc = 0;
out:
    mt_toml_free(t);
    if (rc) mt_manifest_free(m);
    return rc;
}

void mt_manifest_free(MtManifest *m) {
    free(m->id), free(m->name), free(m->version), free(m->description), free(m->license), free(m->homepage), free(m->icon);
    free(m->loader), free(m->entry), free(m->data), free(m->authors);
    mt_strs_free(&m->games);
    mt_strs_free(&m->platforms);
    mt_strs_free(&m->network);
    for (int i = 0; i < m->ndeps; i++) free(m->deps[i].id), free(m->deps[i].range);
    free(m->deps);
    for (int i = 0; i < m->nsettings; i++) {
        MtSetting *s = &m->settings[i];
        free((char *)s->info.key), free((char *)s->info.label), free((char *)s->info.description), free(s->def_text);
        mt_strs_free(&s->choices);
    }
    free(m->settings);
    for (int i = 0; i < m->nhooks; i++) {
        for (int j = 0; j < m->hooks[i].n; j++) free(m->hooks[i].h[j].name), free(m->hooks[i].h[j].target);
        free(m->hooks[i].h);
        free(m->hooks[i].game);
    }
    free(m->hooks);
    memset(m, 0, sizeof *m);
}

const MtHookSet *mt_manifest_hooks_for(const MtManifest *m, const char *game) {
    for (int i = 0; game && i < m->nhooks; i++)
        if (!strcmp(m->hooks[i].game, game)) return &m->hooks[i];
    return NULL;
}
