/* Modtopia core: shared internals (not part of the SDK). MIT. */
#ifndef MT_INTERNAL_H
#define MT_INTERNAL_H

#include "modtopia/modtopia_host.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- memory and strings */
void *mt_alloc(size_t n);                 /* zeroed; aborts when out of memory */
void *mt_grow(void *p, size_t n);
char *mt_strdup(const char *s);
char *mt_strndup(const char *s, size_t n);
char *mt_fmt(const char *fmt, ...);
char *mt_vfmt(const char *fmt, va_list ap);
int mt_streq(const char *a, const char *b);   /* NULL-safe */
int mt_ends_with(const char *s, const char *suffix);   /* ASCII case-insensitive */
void mt_lower(char *s);

typedef struct MtBuf { char *p; size_t n, cap; } MtBuf;
void mt_buf_add(MtBuf *b, const void *p, size_t n);
void mt_buf_str(MtBuf *b, const char *s);
void mt_buf_fmt(MtBuf *b, const char *fmt, ...);
char *mt_buf_take(MtBuf *b);   /* NUL-terminated; the buffer is empty after */

/* A list of strings */
typedef struct MtStrs { char **v; int n, cap; } MtStrs;
void mt_strs_add(MtStrs *l, const char *s);
int mt_strs_find(const MtStrs *l, const char *s);
void mt_strs_remove(MtStrs *l, int i);
void mt_strs_insert(MtStrs *l, int at, const char *s);
void mt_strs_free(MtStrs *l);

/* ---------------------------------------------------------------- files (UTF-8 paths everywhere; wide calls on Windows) */
char *mt_path_join(const char *a, const char *b);
char *mt_path_abs(const char *p);
const char *mt_path_base(const char *p);   /* the last component */
int mt_is_dir(const char *p);
int mt_is_file(const char *p);
int mt_mkdirs(const char *p);
char *mt_read_file(const char *p, size_t *n);   /* NUL-terminated; NULL on failure */
int mt_write_file(const char *p, const void *data, size_t n);   /* through a temporary file and a rename */
int mt_file_info(const char *p, uint64_t *size, int64_t *mtime);
int mt_remove_tree(const char *p);
FILE *mt_fopen(const char *p, const char *mode);
int mt_rename(const char *from, const char *to);
int mt_remove_file(const char *p);
typedef int (*MtDirFn)(void *user, const char *name, int is_dir);
int mt_list_dir(const char *p, MtDirFn fn, void *user);   /* sorted by name; "." and ".." left out */
/* every file under root, relative paths with '/' */
int mt_walk_files(const char *root, void (*fn)(void *user, const char *rel), void *user);

/* ---------------------------------------------------------------- dynamic libraries */
void *mt_lib_open(const char *path, char *err, size_t errn);
void *mt_lib_sym(void *lib, const char *name);

/* ---------------------------------------------------------------- a lock and a once-flag */
typedef struct MtLock { void *p; } MtLock;
void mt_lock_init(MtLock *l);
void mt_lock(MtLock *l);
void mt_unlock(MtLock *l);

/* ---------------------------------------------------------------- TOML (the subset mod.toml, config and mappings use: no dates) */
typedef enum { MT_T_NONE = 0, MT_T_STRING, MT_T_INT, MT_T_FLOAT, MT_T_BOOL, MT_T_ARRAY, MT_T_TABLE } MtTomlType;
typedef struct MtToml MtToml;
struct MtToml {
    MtTomlType type;
    char *s;
    int64_t i;
    double f;
    int b;
    int n, cap;
    char **keys;        /* tables */
    MtToml **items;     /* tables and arrays */
    int explicit_;      /* a table defined by a [header] or a key, not only implied by a path */
    int aot;            /* an array of tables ([[...]]) */
    int line;
};
MtToml *mt_toml_parse(const char *text, char *err, size_t errn);
void mt_toml_free(MtToml *t);
MtToml *mt_toml_get(const MtToml *table, const char *key);   /* one key (no dots) */
const char *mt_toml_str(const MtToml *table, const char *key, const char *def);
int64_t mt_toml_int(const MtToml *table, const char *key, int64_t def);
double mt_toml_num(const MtToml *table, const char *key, double def);   /* int or float */
int mt_toml_bool(const MtToml *table, const char *key, int def);
void mt_toml_quote(MtBuf *b, const char *s);   /* a basic string, escaped */

/* ---------------------------------------------------------------- versions */
typedef struct MtVer { int64_t major, minor, patch; char pre[64]; int parts; } MtVer;
int mt_ver_parse(const char *s, MtVer *v, int partial);
int mt_ver_cmp(const MtVer *a, const MtVer *b);

/* ---------------------------------------------------------------- checksums, zip */
void mt_sha256(const void *data, size_t n, uint8_t out[32]);
void mt_sha256_hex(const void *data, size_t n, char out[65]);
typedef struct MtSha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t nbuf; } MtSha256;
void mt_sha256_init(MtSha256 *s);
void mt_sha256_update(MtSha256 *s, const void *data, size_t n);
void mt_sha256_final(MtSha256 *s, uint8_t out[32]);
uint32_t mt_crc32(uint32_t crc, const void *data, size_t n);
/* raw DEFLATE (RFC 1951) into out[0..outn): 0, or -1 on bad data or a size mismatch */
int mt_inflate(const uint8_t *in, size_t inn, uint8_t *out, size_t outn);

typedef struct MtZipEntry { char *name; uint32_t crc, method, flags; uint64_t csize, usize, offset; int is_dir; } MtZipEntry;
typedef struct MtZip { uint8_t *data; size_t size; MtZipEntry *e; int n; } MtZip;
int mt_zip_open(MtZip *z, const char *path, char *err, size_t errn);   /* reads the whole file */
void mt_zip_close(MtZip *z);
int mt_zip_find(const MtZip *z, const char *name);
uint8_t *mt_zip_read(const MtZip *z, int i, size_t *n, char *err, size_t errn);   /* NUL-terminated */
/* every entry under prefix ("" all) into dir, the prefix removed; refuses unsafe names */
int mt_zip_extract(const MtZip *z, const char *prefix, const char *dir, char *err, size_t errn);
int mt_zip_safe_name(const char *name);

/* ---------------------------------------------------------------- log */
void mt_log(MtLoader *L, int level, const char *fmt, ...);

/* ---------------------------------------------------------------- pictures (mt_png.c) */
uint8_t *mt_png_rgba(const uint8_t *png, size_t n, int *w, int *h);                    /* malloc'd RGBA, or NULL */
void mt_png_fit(const uint8_t *src, int w, int h, uint8_t *out, int size);             /* into size x size RGBA */

/* ---------------------------------------------------------------- the network for mods (mt_net.c) */
int mt_net_host_valid(const char *host);
int mt_net_url_allowed(const MtStrs *hosts, const char *url);
void mt_net_init(void);
void mt_net_frame(MtLoader *L);
void mt_net_forget(MtLoader *L);

#endif
