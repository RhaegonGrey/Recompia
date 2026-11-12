/* Modtopia core: memory, strings, files, libraries and locks on Windows and POSIX. MIT. */
#include "mt_internal.h"

#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- memory and strings */
void *mt_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fputs("modtopia: out of memory\n", stderr); abort(); }
    return p;
}
void *mt_grow(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("modtopia: out of memory\n", stderr); abort(); }
    return q;
}
char *mt_strdup(const char *s) { return s ? mt_strndup(s, strlen(s)) : NULL; }
char *mt_strndup(const char *s, size_t n) {
    char *r = mt_alloc(n + 1);
    memcpy(r, s, n);
    return r;
}
char *mt_vfmt(const char *fmt, va_list ap) {
    va_list c;
    va_copy(c, ap);
    int n = vsnprintf(NULL, 0, fmt, c);
    va_end(c);
    if (n < 0) return mt_strdup("");
    char *r = mt_alloc((size_t)n + 1);
    vsnprintf(r, (size_t)n + 1, fmt, ap);
    return r;
}
char *mt_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *r = mt_vfmt(fmt, ap);
    va_end(ap);
    return r;
}
int mt_streq(const char *a, const char *b) { return a && b ? !strcmp(a, b) : a == b; }
int mt_ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}
void mt_lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

void mt_buf_add(MtBuf *b, const void *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = mt_grow(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}
void mt_buf_str(MtBuf *b, const char *s) { mt_buf_add(b, s, strlen(s)); }
void mt_buf_fmt(MtBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = mt_vfmt(fmt, ap);
    va_end(ap);
    mt_buf_str(b, s);
    free(s);
}
char *mt_buf_take(MtBuf *b) {
    char *r = b->p ? b->p : mt_strdup("");
    b->p = NULL;
    b->n = b->cap = 0;
    return r;
}

void mt_strs_add(MtStrs *l, const char *s) { mt_strs_insert(l, l->n, s); }
void mt_strs_insert(MtStrs *l, int at, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = mt_grow(l->v, sizeof *l->v * (size_t)l->cap);
    }
    if (at < 0) at = 0;
    if (at > l->n) at = l->n;
    memmove(l->v + at + 1, l->v + at, sizeof *l->v * (size_t)(l->n - at));
    l->v[at] = mt_strdup(s);
    l->n++;
}
int mt_strs_find(const MtStrs *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) return i;
    return -1;
}
void mt_strs_remove(MtStrs *l, int i) {
    if (i < 0 || i >= l->n) return;
    free(l->v[i]);
    memmove(l->v + i, l->v + i + 1, sizeof *l->v * (size_t)(l->n - i - 1));
    l->n--;
}
void mt_strs_free(MtStrs *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* ---------------------------------------------------------------- paths */
static int is_sep(char c) { return c == '/' || c == '\\'; }
char *mt_path_join(const char *a, const char *b) {
    if (!a || !*a) return mt_strdup(b);
    size_t n = strlen(a);
    while (*b && is_sep(*b)) b++;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    return mt_fmt("%s%s%s", a, n && is_sep(a[n - 1]) ? "" : sep, b);
}
const char *mt_path_base(const char *p) {
    const char *r = p;
    for (const char *s = p; *s; s++)
        if (is_sep(*s) && s[1]) r = s + 1;
    return r;
}

#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = mt_alloc(sizeof *w * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = mt_alloc((size_t)(n > 0 ? n : 1));
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
char *mt_path_abs(const char *p) {
    wchar_t *w = wide(p), buf[4096];
    DWORD n = GetFullPathNameW(w, 4096, buf, NULL);
    free(w);
    return n && n < 4096 ? narrow(buf) : mt_strdup(p);
}
static DWORD attrs(const char *p) {
    wchar_t *w = wide(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}
int mt_is_dir(const char *p) { DWORD a = attrs(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
int mt_is_file(const char *p) { DWORD a = attrs(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int mkdir1(const char *p) {
    wchar_t *w = wide(p);
    int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}
FILE *mt_fopen(const char *p, const char *mode) {
    wchar_t *w = wide(p), *m = wide(mode);
    FILE *f = _wfopen(w, m);
    free(w);
    free(m);
    return f;
}
int mt_rename(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    int ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok ? 0 : -1;
}
int mt_remove_file(const char *p) {
    wchar_t *w = wide(p);
    int ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}
int mt_file_info(const char *p, uint64_t *size, int64_t *mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = wide(p);
    int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return -1;
    if (size) *size = (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    if (mtime) *mtime = (int64_t)(((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime) / 10000000ull);
    return 0;
}
typedef struct { char *name; int dir; } dent;
static int dent_cmp(const void *a, const void *b) { return strcmp(((const dent *)a)->name, ((const dent *)b)->name); }
int mt_list_dir(const char *p, MtDirFn fn, void *user) {
    char *pat = mt_path_join(p, "*");
    wchar_t *w = wide(pat);
    free(pat);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(w, &d);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    dent *v = NULL;
    int n = 0, cap = 0;
    do {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
        if (n == cap) v = mt_grow(v, sizeof *v * (size_t)(cap = cap ? cap * 2 : 16));
        v[n].name = narrow(d.cFileName);
        v[n++].dir = !!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    qsort(v, (size_t)n, sizeof *v, dent_cmp);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (!rc) rc = fn(user, v[i].name, v[i].dir);
        free(v[i].name);
    }
    free(v);
    return rc;
}
static int rm_entry(void *user, const char *name, int is_dir) {
    char *c = mt_path_join(user, name);
    if (is_dir) mt_remove_tree(c);
    else {
        wchar_t *w = wide(c);
        SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(w);
        free(w);
    }
    free(c);
    return 0;
}
int mt_remove_tree(const char *p) {
    if (!mt_is_dir(p)) return 0;
    mt_list_dir(p, rm_entry, (void *)p);
    wchar_t *w = wide(p);
    int ok = RemoveDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}
void *mt_lib_open(const char *path, char *err, size_t errn) {
    wchar_t *w = wide(path);
    /* the library's own folder first, for DLLs it ships beside itself */
    HMODULE h = LoadLibraryExW(w, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    free(w);
    if (!h && err) snprintf(err, errn, "Windows error %lu", GetLastError());
    return h;
}
void *mt_lib_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
void mt_lock_init(MtLock *l) {
    SRWLOCK *s = mt_alloc(sizeof *s);
    InitializeSRWLock(s);
    l->p = s;
}
void mt_lock(MtLock *l) { AcquireSRWLockExclusive((SRWLOCK *)l->p); }
void mt_unlock(MtLock *l) { ReleaseSRWLockExclusive((SRWLOCK *)l->p); }
#else
char *mt_path_abs(const char *p) {
    char buf[PATH_MAX];
    if (realpath(p, buf)) return mt_strdup(buf);
    if (p[0] == '/') return mt_strdup(p);
    if (getcwd(buf, sizeof buf)) return mt_path_join(buf, p);
    return mt_strdup(p);
}
int mt_is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
int mt_is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int mkdir1(const char *p) { return mkdir(p, 0755) && errno != EEXIST ? -1 : 0; }
FILE *mt_fopen(const char *p, const char *mode) { return fopen(p, mode); }
int mt_rename(const char *from, const char *to) { return rename(from, to); }
int mt_remove_file(const char *p) { return remove(p); }
int mt_file_info(const char *p, uint64_t *size, int64_t *mtime) {
    struct stat st;
    if (stat(p, &st)) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
}
typedef struct { char *name; int dir; } dent;
static int dent_cmp(const void *a, const void *b) { return strcmp(((const dent *)a)->name, ((const dent *)b)->name); }
int mt_list_dir(const char *p, MtDirFn fn, void *user) {
    DIR *d = opendir(p);
    if (!d) return -1;
    dent *v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) v = mt_grow(v, sizeof *v * (size_t)(cap = cap ? cap * 2 : 16));
        char *full = mt_path_join(p, e->d_name);
        v[n].name = mt_strdup(e->d_name);
        v[n++].dir = mt_is_dir(full);
        free(full);
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, dent_cmp);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (!rc) rc = fn(user, v[i].name, v[i].dir);
        free(v[i].name);
    }
    free(v);
    return rc;
}
static int rm_entry(void *user, const char *name, int is_dir) {
    char *c = mt_path_join(user, name);
    if (is_dir) mt_remove_tree(c);
    else unlink(c);
    free(c);
    return 0;
}
int mt_remove_tree(const char *p) {
    if (!mt_is_dir(p)) return 0;
    mt_list_dir(p, rm_entry, (void *)p);
    return rmdir(p);
}
void *mt_lib_open(const char *path, char *err, size_t errn) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h && err) snprintf(err, errn, "%s", dlerror());
    return h;
}
void *mt_lib_sym(void *lib, const char *name) { return dlsym(lib, name); }
void mt_lock_init(MtLock *l) {
    pthread_mutex_t *m = mt_alloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    l->p = m;
}
void mt_lock(MtLock *l) { pthread_mutex_lock((pthread_mutex_t *)l->p); }
void mt_unlock(MtLock *l) { pthread_mutex_unlock((pthread_mutex_t *)l->p); }
#endif

int mt_mkdirs(const char *p) {
    if (!p || !*p || mt_is_dir(p)) return 0;
    char *c = mt_strdup(p);
    for (char *s = c + 1; *s; s++) {
        if (!is_sep(*s)) continue;
        if (s > c && s[-1] == ':') continue;   /* "C:\" */
        char k = *s;
        *s = 0;
        if (!mt_is_dir(c)) mkdir1(c);
        *s = k;
    }
    int rc = mkdir1(c);
    free(c);
    return rc;
}

FILE *mt_fopen(const char *p, const char *mode);
char *mt_read_file(const char *p, size_t *n) {
    FILE *f = mt_fopen(p, "rb");
    if (!f) return NULL;
    MtBuf b = {0};
    char tmp[65536];
    size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) mt_buf_add(&b, tmp, k);
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(b.p); return NULL; }
    if (n) *n = b.n;
    return mt_buf_take(&b);
}
int mt_write_file(const char *p, const void *data, size_t n) {
    char *tmp = mt_fmt("%s.tmp%u", p, (unsigned)rand());
    FILE *f = mt_fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    int ok = fwrite(data, 1, n, f) == n;
    ok = !fclose(f) && ok;
    if (ok) ok = !mt_rename(tmp, p);
    if (!ok) {
#ifdef _WIN32
        wchar_t *w = wide(tmp);
        DeleteFileW(w);
        free(w);
#else
        unlink(tmp);
#endif
    }
    free(tmp);
    return ok ? 0 : -1;
}

typedef struct { const char *root; char *rel; void (*fn)(void *, const char *); void *user; } walk_t;
static int walk_entry(void *u, const char *name, int is_dir) {
    walk_t *w = u;
    char *rel = w->rel[0] ? mt_fmt("%s/%s", w->rel, name) : mt_strdup(name);
    if (is_dir) {
        char *full = mt_path_join(w->root, rel);
        walk_t sub = {w->root, rel, w->fn, w->user};
        mt_list_dir(full, walk_entry, &sub);
        free(full);
    } else {
        w->fn(w->user, rel);
    }
    free(rel);
    return 0;
}
int mt_walk_files(const char *root, void (*fn)(void *user, const char *rel), void *user) {
    walk_t w = {root, (char *)"", fn, user};
    return mt_list_dir(root, walk_entry, &w);
}
