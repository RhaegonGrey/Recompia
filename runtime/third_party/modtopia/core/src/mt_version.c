/* Modtopia core: semantic versions and the ranges manifests use (">=1.2 <2", "^1.4.0", "~0.3", "1.x", "*", "a || b"). MIT. */
#include "mt_internal.h"

#include <ctype.h>

/* "1.2.3-pre+build". partial: "1" and "1.2" and wildcards (x, X, *) are allowed; parts = how many numbers were given */
int mt_ver_parse(const char *s, MtVer *v, int partial) {
    memset(v, 0, sizeof *v);
    while (*s == ' ') s++;
    if (*s == 'v' || *s == 'V') s++;
    int64_t *f[3] = {&v->major, &v->minor, &v->patch};
    for (int i = 0; i < 3; i++) {
        if (partial && (*s == 'x' || *s == 'X' || *s == '*')) { s++; break; }
        if (!isdigit((unsigned char)*s)) return i && partial ? 0 : -1;
        if (*s == '0' && isdigit((unsigned char)s[1])) return -1;   /* no leading zeros */
        int64_t n = 0;
        while (isdigit((unsigned char)*s)) {
            if (n > 100000000) return -1;
            n = n * 10 + (*s++ - '0');
        }
        *f[i] = n;
        v->parts = i + 1;
        if (i < 2) {
            if (*s != '.') return partial ? (*s && *s != '-' && *s != '+' && *s != ' ' ? -1 : 0) : -1;
            s++;
        }
    }
    if (*s == '-') {
        s++;
        size_t k = 0;
        while (*s && *s != '+' && *s != ' ' && k + 1 < sizeof v->pre) {
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') return -1;
            v->pre[k++] = *s++;
        }
        if (!k) return -1;
    }
    if (*s == '+') {   /* build metadata: ignored for order */
        s++;
        while (*s && *s != ' ') s++;
    }
    while (*s == ' ') s++;
    return *s ? -1 : 0;
}

static int pre_cmp(const char *a, const char *b) {
    if (!*a || !*b) return !*a && !*b ? 0 : !*a ? 1 : -1;   /* a release is above its pre-releases */
    while (*a && *b) {
        const char *ea = strchr(a, '.'), *eb = strchr(b, '.');
        size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        int na = 1, nb = 1;
        for (size_t i = 0; i < la; i++) na &= isdigit((unsigned char)a[i]) != 0;
        for (size_t i = 0; i < lb; i++) nb &= isdigit((unsigned char)b[i]) != 0;
        int c;
        if (na && nb) c = la != lb ? (la < lb ? -1 : 1) : strncmp(a, b, la);
        else if (na != nb) c = na ? -1 : 1;
        else {
            c = strncmp(a, b, la < lb ? la : lb);
            if (!c) c = la < lb ? -1 : la > lb;
        }
        if (c) return c < 0 ? -1 : 1;
        a += la + (ea ? 1 : 0);
        b += lb + (eb ? 1 : 0);
        if (!ea || !eb) return !ea && !eb ? 0 : !ea ? -1 : 1;
    }
    return 0;
}
int mt_ver_cmp(const MtVer *a, const MtVer *b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return pre_cmp(a->pre, b->pre);
}
int mt_version_compare(const char *a, const char *b) {
    MtVer x, y;
    if (mt_ver_parse(a, &x, 0) || mt_ver_parse(b, &y, 0)) return strcmp(a, b);
    return mt_ver_cmp(&x, &y);
}

/* One comparator ("op version") against v: 1, 0, or -1 when malformed */
static int comparator(const char *tok, const MtVer *v) {
    char op[3] = {0};
    size_t k = 0;
    while (k < 2 && strchr("<>=^~", tok[k])) { op[k] = tok[k]; k++; }
    MtVer r;
    if (mt_ver_parse(tok + k, &r, 1)) return -1;
    MtVer lo = r, hi = {0};
    int has_hi = 0;
    /* a partial version is a range by itself: "1.2" = >=1.2.0 <1.3.0, "1" = >=1.0.0 <2.0.0, "*" = anything */
    if (!op[0] || !strcmp(op, "=")) {
        if (r.parts == 0) return 1;
        if (r.parts == 3) return mt_ver_cmp(v, &r) == 0;
        hi = r;
        if (r.parts == 1) hi.major++, hi.minor = 0; else hi.minor++;
        hi.patch = 0;
        has_hi = 1;
    } else if (!strcmp(op, "^")) {
        hi = r;
        hi.pre[0] = 0;
        if (r.major || r.parts == 1) hi.major++, hi.minor = 0, hi.patch = 0;
        else if (r.minor || r.parts == 2) hi.minor++, hi.patch = 0;
        else hi.patch++;
        has_hi = 1;
    } else if (!strcmp(op, "~")) {
        hi = r;
        hi.pre[0] = 0;
        if (r.parts == 1) hi.major++, hi.minor = 0; else hi.minor++;
        hi.patch = 0;
        has_hi = 1;
    } else {
        int c = mt_ver_cmp(v, &r);
        if (!strcmp(op, ">=")) return c >= 0;
        if (!strcmp(op, "<=")) return c <= 0;
        if (!strcmp(op, ">")) return c > 0;
        if (!strcmp(op, "<")) return c < 0;
        return -1;
    }
    if (mt_ver_cmp(v, &lo) < 0) return 0;
    if (has_hi) {
        if (mt_ver_cmp(v, &hi) >= 0) return 0;
        if (!hi.pre[0] && v->pre[0] && v->major == hi.major && v->minor == hi.minor && v->patch == hi.patch) return 0;
    }
    return 1;
}

int mt_version_satisfies(const char *version, const char *range) {
    MtVer v;
    if (mt_ver_parse(version, &v, 0)) return -1;
    if (!range || !*range) return 1;
    char *copy = mt_strdup(range);
    int any = 0, bad = 0;
    char *alt = copy;
    while (alt && !bad) {   /* "a || b": either */
        char *next = strstr(alt, "||");
        if (next) *next = 0;
        int all = 1, seen = 0;
        char *s = alt;
        while (*s && !bad) {   /* comparators separated by spaces or commas: all of them */
            while (*s == ' ' || *s == ',') s++;
            if (!*s) break;
            char tok[96];
            size_t k = 0;
            while (*s && strchr("<>=^~", *s) && k + 1 < sizeof tok) tok[k++] = *s++;
            while (*s == ' ') s++;
            while (*s && *s != ' ' && *s != ',' && k + 1 < sizeof tok) tok[k++] = *s++;
            tok[k] = 0;
            int r = comparator(tok, &v);
            if (r < 0) bad = 1;
            else all &= r, seen = 1;
        }
        if (!seen) bad = 1;
        if (all && !bad) any = 1;
        alt = next ? next + 2 : NULL;
    }
    free(copy);
    return bad ? -1 : any;
}
