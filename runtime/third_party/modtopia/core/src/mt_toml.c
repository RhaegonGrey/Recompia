/* Modtopia core: a TOML 1.0 reader for what manifests, settings and mappings use (everything but dates and times). MIT. */
#include "mt_internal.h"

#include <ctype.h>
#include <math.h>

typedef struct {
    const char *p, *end;
    int line;
    char *err;
    size_t errn;
    int failed;
} P;

static void fail(P *p, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = 1;
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (p->err) snprintf(p->err, p->errn, "line %d: %s", p->line, msg);
}

static MtToml *node(MtTomlType t, int line) {
    MtToml *n = mt_alloc(sizeof *n);
    n->type = t;
    n->line = line;
    return n;
}
void mt_toml_free(MtToml *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        mt_toml_free(t->items[i]);
        if (t->keys) free(t->keys[i]);
    }
    free(t->items);
    free(t->keys);
    free(t->s);
    free(t);
}
static void push(MtToml *t, char *key, MtToml *v) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->items = mt_grow(t->items, sizeof *t->items * (size_t)t->cap);
        if (t->type == MT_T_TABLE) t->keys = mt_grow(t->keys, sizeof *t->keys * (size_t)t->cap);
    }
    if (t->type == MT_T_TABLE) t->keys[t->n] = key;
    t->items[t->n++] = v;
}
MtToml *mt_toml_get(const MtToml *t, const char *key) {
    if (!t || t->type != MT_T_TABLE) return NULL;
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->keys[i], key)) return t->items[i];
    return NULL;
}

/* ---------------------------------------------------------------- lexing */
static int at_end(P *p) { return p->p >= p->end; }
static void skip_ws(P *p) { while (!at_end(p) && (*p->p == ' ' || *p->p == '\t')) p->p++; }
static void skip_comment(P *p) {
    if (!at_end(p) && *p->p == '#')
        while (!at_end(p) && *p->p != '\n') p->p++;
}
static int eol(P *p) {   /* the end of a line (after a key/value or header): spaces, a comment, then a newline or the end */
    skip_ws(p);
    skip_comment(p);
    if (at_end(p)) return 1;
    if (*p->p == '\r' && p->p + 1 < p->end && p->p[1] == '\n') p->p++;
    if (*p->p == '\n') { p->p++; p->line++; return 1; }
    return 0;
}
static void skip_blank(P *p) {   /* spaces, comments and newlines (inside arrays) */
    for (;;) {
        skip_ws(p);
        skip_comment(p);
        if (at_end(p)) return;
        if (*p->p == '\r') { p->p++; continue; }
        if (*p->p == '\n') { p->p++; p->line++; continue; }
        return;
    }
}
static void utf8_put(MtBuf *b, uint32_t c) {
    char o[4];
    int n;
    if (c < 0x80) o[0] = (char)c, n = 1;
    else if (c < 0x800) o[0] = (char)(0xC0 | c >> 6), o[1] = (char)(0x80 | (c & 63)), n = 2;
    else if (c < 0x10000) o[0] = (char)(0xE0 | c >> 12), o[1] = (char)(0x80 | (c >> 6 & 63)), o[2] = (char)(0x80 | (c & 63)), n = 3;
    else o[0] = (char)(0xF0 | c >> 18), o[1] = (char)(0x80 | (c >> 12 & 63)), o[2] = (char)(0x80 | (c >> 6 & 63)), o[3] = (char)(0x80 | (c & 63)), n = 4;
    mt_buf_add(b, o, (size_t)n);
}
static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : isxdigit((unsigned char)c) ? (tolower((unsigned char)c) - 'a' + 10) : -1; }

/* A string at p (", ', """ or '''): its value, NULL after fail() */
static char *string(P *p) {
    char q = *p->p;
    int multi = p->end - p->p >= 3 && p->p[1] == q && p->p[2] == q;
    p->p += multi ? 3 : 1;
    if (multi) {   /* a newline right after the opening quotes is dropped */
        if (!at_end(p) && *p->p == '\r') p->p++;
        if (!at_end(p) && *p->p == '\n') { p->p++; p->line++; }
    }
    MtBuf b = {0};
    mt_buf_add(&b, "", 0);
    for (;;) {
        if (at_end(p)) { fail(p, "a string is not closed"); free(b.p); return NULL; }
        char c = *p->p;
        if (c == q) {
            if (!multi) { p->p++; break; }
            if (p->end - p->p >= 3 && p->p[1] == q && p->p[2] == q) {
                p->p += 3;
                while (!at_end(p) && *p->p == q) { mt_buf_add(&b, &q, 1); p->p++; }   /* up to two quotes may end the content */
                break;
            }
        }
        if (c == '\n') {
            if (!multi) { fail(p, "a newline inside a one-line string"); free(b.p); return NULL; }
            p->line++;
        }
        if (c == '\\' && q == '"') {
            p->p++;
            if (at_end(p)) continue;
            char e = *p->p++;
            switch (e) {
            case 'b': mt_buf_add(&b, "\b", 1); break;
            case 't': mt_buf_add(&b, "\t", 1); break;
            case 'n': mt_buf_add(&b, "\n", 1); break;
            case 'f': mt_buf_add(&b, "\f", 1); break;
            case 'r': mt_buf_add(&b, "\r", 1); break;
            case 'e': mt_buf_add(&b, "\x1b", 1); break;
            case '"': mt_buf_add(&b, "\"", 1); break;
            case '\\': mt_buf_add(&b, "\\", 1); break;
            case 'u':
            case 'U': {
                int k = e == 'u' ? 4 : 8;
                uint32_t v = 0;
                for (int i = 0; i < k; i++) {
                    int h = at_end(p) ? -1 : hexval(*p->p);
                    if (h < 0) { fail(p, "a bad \\%c escape", e); free(b.p); return NULL; }
                    v = v << 4 | (uint32_t)h;
                    p->p++;
                }
                if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { fail(p, "an escape that is no Unicode character"); free(b.p); return NULL; }
                utf8_put(&b, v);
                break;
            }
            default:
                if (multi && (e == ' ' || e == '\t' || e == '\r' || e == '\n')) {   /* a line-ending backslash trims to the next text */
                    p->p--;
                    while (!at_end(p) && strchr(" \t\r\n", *p->p)) { if (*p->p == '\n') p->line++; p->p++; }
                    break;
                }
                fail(p, "an unknown escape \\%c", e);
                free(b.p);
                return NULL;
            }
            continue;
        }
        mt_buf_add(&b, &c, 1);
        p->p++;
    }
    return mt_buf_take(&b);
}

static int bare_key_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }
static char *simple_key(P *p) {
    if (at_end(p)) { fail(p, "a key is missing"); return NULL; }
    if (*p->p == '"' || *p->p == '\'') {
        if (p->end - p->p >= 3 && p->p[1] == *p->p && p->p[2] == *p->p) { fail(p, "a key cannot be a multi-line string"); return NULL; }
        return string(p);
    }
    const char *s = p->p;
    while (!at_end(p) && bare_key_char(*p->p)) p->p++;
    if (p->p == s) { fail(p, "a key is missing or has a character it cannot have"); return NULL; }
    return mt_strndup(s, (size_t)(p->p - s));
}
/* a dotted key: its parts */
static int key_path(P *p, MtStrs *out) {
    for (;;) {
        skip_ws(p);
        char *k = simple_key(p);
        if (!k) return -1;
        mt_strs_add(out, k);
        free(k);
        skip_ws(p);
        if (!at_end(p) && *p->p == '.') { p->p++; continue; }
        return 0;
    }
}

static MtToml *value(P *p);

static MtToml *number(P *p) {
    const char *s = p->p;
    while (!at_end(p) && (isalnum((unsigned char)*p->p) || strchr("+-._", *p->p))) p->p++;
    char buf[128];
    size_t k = 0;
    for (const char *c = s; c < p->p && k + 1 < sizeof buf; c++)
        if (*c != '_') buf[k++] = *c;
    buf[k] = 0;
    if (!k) { fail(p, "a value is missing"); return NULL; }
    if (!strcmp(buf, "true") || !strcmp(buf, "false")) {
        MtToml *n = node(MT_T_BOOL, p->line);
        n->b = buf[0] == 't';
        return n;
    }
    const char *b = buf;
    int neg = 0;
    if (*b == '+' || *b == '-') neg = *b++ == '-';
    if (!strcmp(b, "inf") || !strcmp(b, "nan")) {
        MtToml *n = node(MT_T_FLOAT, p->line);
        n->f = b[0] == 'i' ? (neg ? -INFINITY : INFINITY) : NAN;
        return n;
    }
    if (b[0] == '0' && (b[1] == 'x' || b[1] == 'o' || b[1] == 'b')) {
        int base = b[1] == 'x' ? 16 : b[1] == 'o' ? 8 : 2;
        char *e;
        uint64_t v = strtoull(b + 2, &e, base);
        if (*e || !b[2] || neg) { fail(p, "a bad number: %s", buf); return NULL; }
        MtToml *n = node(MT_T_INT, p->line);
        n->i = (int64_t)v;
        return n;
    }
    if (!isdigit((unsigned char)*b)) { fail(p, "a value TOML does not have: %s", buf); return NULL; }
    if (strpbrk(b, ".eE")) {
        char *e;
        double v = strtod(buf, &e);
        if (*e) { fail(p, "a bad number: %s", buf); return NULL; }
        MtToml *n = node(MT_T_FLOAT, p->line);
        n->f = v;
        return n;
    }
    if (strchr(b, '-') || strchr(b, ':')) { fail(p, "dates and times are not supported: %s", buf); return NULL; }
    char *e;
    long long v = strtoll(buf, &e, 10);
    if (*e) { fail(p, "a bad number: %s", buf); return NULL; }
    MtToml *n = node(MT_T_INT, p->line);
    n->i = v;
    return n;
}

static int set_path(P *p, MtToml *root, MtStrs *path, MtToml *v);

static MtToml *inline_table(P *p) {
    p->p++;
    MtToml *t = node(MT_T_TABLE, p->line);
    t->explicit_ = 1;
    skip_ws(p);
    if (!at_end(p) && *p->p == '}') { p->p++; return t; }
    for (;;) {
        MtStrs path = {0};
        if (key_path(p, &path)) { mt_strs_free(&path); mt_toml_free(t); return NULL; }
        skip_ws(p);
        if (at_end(p) || *p->p != '=') { fail(p, "'=' is missing after a key"); mt_strs_free(&path); mt_toml_free(t); return NULL; }
        p->p++;
        skip_ws(p);
        MtToml *v = value(p);
        if (!v || set_path(p, t, &path, v)) { mt_toml_free(v); mt_strs_free(&path); mt_toml_free(t); return NULL; }
        mt_strs_free(&path);
        skip_ws(p);
        if (!at_end(p) && *p->p == ',') { p->p++; skip_ws(p); continue; }
        if (!at_end(p) && *p->p == '}') { p->p++; return t; }
        fail(p, "an inline table is not closed with '}'");
        mt_toml_free(t);
        return NULL;
    }
}

static MtToml *array(P *p) {
    p->p++;
    MtToml *a = node(MT_T_ARRAY, p->line);
    for (;;) {
        skip_blank(p);
        if (at_end(p)) { fail(p, "an array is not closed"); mt_toml_free(a); return NULL; }
        if (*p->p == ']') { p->p++; return a; }
        MtToml *v = value(p);
        if (!v) { mt_toml_free(a); return NULL; }
        push(a, NULL, v);
        skip_blank(p);
        if (!at_end(p) && *p->p == ',') { p->p++; continue; }
        if (!at_end(p) && *p->p == ']') { p->p++; return a; }
        fail(p, "',' or ']' is missing in an array");
        mt_toml_free(a);
        return NULL;
    }
}

static MtToml *value(P *p) {
    if (at_end(p)) { fail(p, "a value is missing"); return NULL; }
    char c = *p->p;
    if (c == '"' || c == '\'') {
        int line = p->line;
        char *s = string(p);
        if (!s) return NULL;
        MtToml *n = node(MT_T_STRING, line);
        n->s = s;
        return n;
    }
    if (c == '[') return array(p);
    if (c == '{') return inline_table(p);
    return number(p);
}

/* The table at path below root, made where missing; aot: the last part is an array of tables and a new table is appended */
static MtToml *walk(P *p, MtToml *root, MtStrs *path, int upto, int define, int aot) {
    MtToml *t = root;
    for (int i = 0; i < upto; i++) {
        MtToml *c = mt_toml_get(t, path->v[i]);
        int last = i == upto - 1;
        if (!c) {
            if (last && aot) {
                c = node(MT_T_ARRAY, p->line);
                c->aot = 1;
            } else {
                c = node(MT_T_TABLE, p->line);
            }
            push(t, mt_strdup(path->v[i]), c);
        }
        if (c->type == MT_T_ARRAY) {
            if (!c->aot) { fail(p, "'%s' is an array, not a table", path->v[i]); return NULL; }
            if (last && aot) {
                MtToml *n = node(MT_T_TABLE, p->line);
                n->explicit_ = 1;
                push(c, NULL, n);
                return n;
            }
            c = c->items[c->n - 1];   /* the latest table of the array */
        } else if (c->type != MT_T_TABLE) {
            fail(p, "'%s' already has a value", path->v[i]);
            return NULL;
        } else if (last && aot) {
            fail(p, "'%s' is a table, not an array of tables", path->v[i]);
            return NULL;
        }
        if (last && define) {
            if (c->explicit_) { fail(p, "the table [%s] is defined twice", path->v[i]); return NULL; }
            c->explicit_ = 1;
        }
        t = c;
    }
    return t;
}
static int set_path(P *p, MtToml *root, MtStrs *path, MtToml *v) {
    MtToml *t = walk(p, root, path, path->n - 1, 0, 0);
    if (!t) return -1;
    const char *k = path->v[path->n - 1];
    if (mt_toml_get(t, k)) { fail(p, "the key '%s' is set twice", k); return -1; }
    push(t, mt_strdup(k), v);
    return 0;
}

MtToml *mt_toml_parse(const char *text, char *err, size_t errn) {
    P p = {text, text + strlen(text), 1, err, errn, 0};
    if (p.end - p.p >= 3 && (uint8_t)p.p[0] == 0xEF && (uint8_t)p.p[1] == 0xBB && (uint8_t)p.p[2] == 0xBF) p.p += 3;   /* a BOM */
    MtToml *root = node(MT_T_TABLE, 1), *cur = root;
    root->explicit_ = 1;
    while (!p.failed) {
        skip_blank(&p);
        if (at_end(&p)) break;
        if (*p.p == '[') {
            int aot = p.p + 1 < p.end && p.p[1] == '[';
            p.p += aot ? 2 : 1;
            MtStrs path = {0};
            if (key_path(&p, &path)) { mt_strs_free(&path); break; }
            if (at_end(&p) || *p.p != ']' || (aot && (p.p + 1 >= p.end || p.p[1] != ']'))) {
                fail(&p, "a table header is not closed");
                mt_strs_free(&path);
                break;
            }
            p.p += aot ? 2 : 1;
            cur = walk(&p, root, &path, path.n, !aot, aot);
            mt_strs_free(&path);
            if (!cur) break;
            if (!eol(&p)) fail(&p, "something after a table header");
            continue;
        }
        MtStrs path = {0};
        if (key_path(&p, &path)) { mt_strs_free(&path); break; }
        skip_ws(&p);
        if (at_end(&p) || *p.p != '=') { fail(&p, "'=' is missing after '%s'", path.v[path.n - 1]); mt_strs_free(&path); break; }
        p.p++;
        skip_ws(&p);
        MtToml *v = value(&p);
        if (!v || set_path(&p, cur, &path, v)) { mt_toml_free(v); mt_strs_free(&path); break; }
        mt_strs_free(&path);
        if (!eol(&p)) fail(&p, "something after a value (one key = value a line)");
    }
    if (p.failed) { mt_toml_free(root); return NULL; }
    return root;
}

const char *mt_toml_str(const MtToml *t, const char *key, const char *def) {
    MtToml *v = mt_toml_get(t, key);
    return v && v->type == MT_T_STRING ? v->s : def;
}
int64_t mt_toml_int(const MtToml *t, const char *key, int64_t def) {
    MtToml *v = mt_toml_get(t, key);
    return v && v->type == MT_T_INT ? v->i : def;
}
double mt_toml_num(const MtToml *t, const char *key, double def) {
    MtToml *v = mt_toml_get(t, key);
    return !v ? def : v->type == MT_T_INT ? (double)v->i : v->type == MT_T_FLOAT ? v->f : def;
}
int mt_toml_bool(const MtToml *t, const char *key, int def) {
    MtToml *v = mt_toml_get(t, key);
    return v && v->type == MT_T_BOOL ? v->b : def;
}
void mt_toml_quote(MtBuf *b, const char *s) {
    mt_buf_str(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') mt_buf_fmt(b, "\\%c", c);
        else if (c == '\n') mt_buf_str(b, "\\n");
        else if (c == '\t') mt_buf_str(b, "\\t");
        else if (c < 0x20 || c == 0x7f) mt_buf_fmt(b, "\\u%04x", c);
        else mt_buf_add(b, s, 1);
    }
    mt_buf_str(b, "\"");
}
