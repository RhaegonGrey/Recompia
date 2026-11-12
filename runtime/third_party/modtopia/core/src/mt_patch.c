/* Modtopia core: patches against the player's own game files (SPEC §10). The MTPATCH1 format, little-endian:

     "MTPATCH1"                        8 bytes
     source size, target size          u64 each
     source sha256, target sha256      32 bytes each
     block                             u32: the matching block its maker used
     reserved                          u32, 0
     then operations, a byte each with LEB128 varints:
       1 COPY  offset length           bytes of the source
       2 ADD   length, the bytes       new bytes
       3 FILL  length, one byte        that byte, length times
       0 END

   A patch carries the change only; what stays the same is copied from the player's own file. Applying one checks the source (size and
   sha256), refuses a patch whose added bytes include more than 16 KiB of the very file it patches (the game's own data, which a patch may not
   carry), and checks the result (size and sha256). Sources and results are streamed, so archives of gigabytes need little memory: the source's
   blocks are indexed by a rolling hash (at most 2M blocks, 32 MiB). MIT. */
#include "mt_patch.h"
#include "mt_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD 96
#define LEGAL_MAX (16u * 1024)
#define CHUNK (1u << 20)
#define FLUSH (4u << 20)
#define POLY 0x01000193u
#define EMPTY 0xffffffffu
#define PROBES 64

enum { OP_END = 0, OP_COPY = 1, OP_ADD = 2, OP_FILL = 3 };

static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }

uint32_t mt_patch_block(uint64_t size) {
    uint32_t b = 32;
    while (size / b > (1u << 21) && b < (1u << 20)) b <<= 1;
    return b;
}

int mt_patch_head(const uint8_t *p, size_t n, MtPatchHead *h) {
    if (n < HEAD || memcmp(p, "MTPATCH1", 8)) return -1;
    h->src_size = rd64(p + 8), h->dst_size = rd64(p + 16);
    memcpy(h->src_sha, p + 24, 32), memcpy(h->dst_sha, p + 56, 32);
    h->block = rd32(p + 88);
    return 0;
}

/* ---------------------------------------------------------------- operations */
typedef struct { int op; uint64_t off, len; const uint8_t *data; uint8_t fill; } Op;
static int varint(const uint8_t *p, size_t n, size_t *at, uint64_t *v) {
    uint64_t r = 0;
    for (int sh = 0; *at < n && sh < 64; sh += 7) {
        uint8_t b = p[(*at)++];
        r |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) { *v = r; return 0; }
    }
    return -1;
}
/* the next operation: 1, 0 at END, -1 when malformed */
static int next_op(const uint8_t *p, size_t n, size_t *at, Op *o) {
    if (*at >= n) return -1;
    o->op = p[(*at)++];
    switch (o->op) {
    case OP_END: return 0;
    case OP_COPY: return varint(p, n, at, &o->off) || varint(p, n, at, &o->len) ? -1 : 1;
    case OP_ADD:
        if (varint(p, n, at, &o->len) || o->len > n - *at) return -1;
        o->data = p + *at, *at += (size_t)o->len;
        return 1;
    case OP_FILL:
        if (varint(p, n, at, &o->len) || *at >= n) return -1;
        o->fill = p[(*at)++];
        return 1;
    }
    return -1;
}
/* NULL when the operations fit the header's sizes, else why not */
static char *walk_check(const uint8_t *p, size_t n, const MtPatchHead *h, uint64_t *max_add) {
    size_t at = HEAD;
    uint64_t total = 0;
    Op o;
    int r;
    if (max_add) *max_add = 0;
    while ((r = next_op(p, n, &at, &o)) == 1) {
        if (o.op == OP_COPY && (o.off > h->src_size || o.len > h->src_size - o.off))
            return mt_fmt("a copy reaches past the end of the original (%llu bytes)", (unsigned long long)h->src_size);
        if (o.len > UINT64_MAX - total) return mt_strdup("its operations overflow");
        total += o.len;
        if (o.op == OP_ADD && max_add && o.len > *max_add) *max_add = o.len;
    }
    if (r < 0) return mt_strdup("its operations are cut short or malformed");
    if (at != n) return mt_strdup("it has bytes after its end");
    if (total != h->dst_size)
        return mt_fmt("its operations make %llu bytes, its header says %llu", (unsigned long long)total, (unsigned long long)h->dst_size);
    return NULL;
}
int mt_patch_check(const uint8_t *p, size_t n, char **why) {
    MtPatchHead h;
    *why = NULL;
    if (mt_patch_head(p, n, &h)) { *why = mt_strdup("not a Modtopia patch (MTPATCH1)"); return -1; }
    *why = walk_check(p, n, &h, NULL);
    return *why ? -1 : 0;
}

/* ---------------------------------------------------------------- the source's blocks, by a rolling hash */
typedef struct { uint32_t *slot, *hash, mask, block; } Index;
static uint32_t pow_last(uint32_t b) { uint32_t r = 1; for (uint32_t i = 1; i < b; i++) r *= POLY; return r; }
static uint32_t hash_of(const uint8_t *p, uint32_t b) { uint32_t h = 0; for (uint32_t i = 0; i < b; i++) h = h * POLY + p[i]; return h; }
static int one_value(const uint8_t *p, uint32_t b) { for (uint32_t i = 1; i < b; i++) if (p[i] != p[0]) return 0; return 1; }
static int index_init(Index *x, uint64_t size, uint32_t block) {
    uint64_t blocks = size / block, slots = 1024;
    while (slots < blocks * 2) slots <<= 1;
    x->block = block, x->mask = (uint32_t)(slots - 1);
    x->slot = malloc(slots * sizeof *x->slot), x->hash = malloc(slots * sizeof *x->hash);
    if (!x->slot || !x->hash) { free(x->slot), free(x->hash); x->slot = x->hash = NULL; return -1; }
    memset(x->slot, 0xff, slots * sizeof *x->slot);
    return 0;
}
static void index_free(Index *x) { free(x->slot), free(x->hash); }
static void index_add(Index *x, uint32_t h, uint32_t k) {
    uint32_t s = (h * 0x9e3779b1u) & x->mask;
    for (int i = 0; i < PROBES && x->slot[s] != EMPTY; i++) s = (s + 1) & x->mask;
    if (x->slot[s] == EMPTY) x->slot[s] = k, x->hash[s] = h;   /* a crowded run drops it: only a match is lost */
}
/* the number of the source block whose bytes are win (block bytes), or -1 */
static int64_t index_find(const Index *x, uint32_t h, const uint8_t *win, MtSrcRead rd, void *ctx, uint8_t *tmp) {
    uint32_t s = (h * 0x9e3779b1u) & x->mask;
    for (int i = 0; i < PROBES && x->slot[s] != EMPTY; i++, s = (s + 1) & x->mask) {
        if (x->hash[s] != h) continue;
        if (rd(ctx, (uint64_t)x->slot[s] * x->block, tmp, x->block) == (int64_t)x->block && !memcmp(tmp, win, x->block)) return x->slot[s];
    }
    return -1;
}
/* Reads the whole source once: its sha256, and (x not NULL) its blocks into the index (one-value blocks, such as padding, left out).
   0, or -1 on a read error */
static int scan_source(uint64_t size, MtSrcRead rd, void *ctx, Index *x, uint8_t sha[32]) {
    uint8_t *buf = malloc(CHUNK);
    uint64_t off = 0;
    MtSha256 s;
    if (!buf) return -1;
    mt_sha256_init(&s);
    while (off < size) {
        uint64_t want = size - off < CHUNK ? size - off : CHUNK;
        if (rd(ctx, off, buf, want) != (int64_t)want) { free(buf); return -1; }
        mt_sha256_update(&s, buf, (size_t)want);
        if (x)
            for (uint64_t b = 0; b + x->block <= want; b += x->block)   /* CHUNK is a multiple of every block size */
                if (!one_value(buf + b, x->block)) index_add(x, hash_of(buf + b, x->block), (uint32_t)((off + b) / x->block));
        off += want;
    }
    mt_sha256_final(&s, sha);
    free(buf);
    return 0;
}
/* how many of the added bytes d[0..len) are whole source blocks */
static uint64_t carried(const Index *x, const uint8_t *d, uint64_t len, MtSrcRead rd, void *ctx, uint8_t *tmp, uint32_t pw) {
    uint32_t B = x->block, h;
    uint64_t i = 0, found = 0;
    if (len < B) return 0;
    h = hash_of(d, B);
    for (;;) {
        if (index_find(x, h, d + i, rd, ctx, tmp) >= 0) {
            found += B, i += B;
            if (len - i < B) break;
            h = hash_of(d + i, B);
            continue;
        }
        if (len - i <= B) break;
        h = (h - d[i] * pw) * POLY + d[i + B];
        i++;
    }
    return found;
}

static char *parent_of(const char *path) {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *e = a > b ? a : b;
    return e ? mt_strndup(path, (size_t)(e - path)) : mt_strdup(".");
}

/* ---------------------------------------------------------------- applying */
int mt_patch_apply(const uint8_t *p, size_t n, uint64_t src_size, MtSrcRead rd, void *ctx, const char *out_path, char **why, int *refused) {
    MtPatchHead h;
    Index x = {0};
    uint64_t max_add = 0;
    uint8_t sha[32], *buf = NULL, *tmp = NULL;
    char *part = NULL, *dir = NULL;
    FILE *f = NULL;
    int ok = -1, legal;
    uint32_t B;
    *why = NULL;
    if (refused) *refused = 0;
    if (mt_patch_head(p, n, &h)) { *why = mt_strdup("not a Modtopia patch (MTPATCH1)"); return -1; }
    if ((*why = walk_check(p, n, &h, &max_add))) return -1;
    if (src_size != h.src_size) {
        *why = mt_fmt("it is for a file of %llu bytes and the game's has %llu (another game version?)", (unsigned long long)h.src_size,
                      (unsigned long long)src_size);
        return -1;
    }
    B = mt_patch_block(src_size);
    legal = max_add >= B;   /* shorter added runs cannot hold a whole block */
    if (legal && index_init(&x, src_size, B)) { *why = mt_strdup("out of memory"); return -1; }
    if (!(buf = malloc(CHUNK)) || !(tmp = malloc(B))) { *why = mt_strdup("out of memory"); goto out; }
    if (scan_source(src_size, rd, ctx, legal ? &x : NULL, sha)) { *why = mt_strdup("the game file could not be read"); goto out; }
    if (memcmp(sha, h.src_sha, 32)) { *why = mt_strdup("the game file is not the one it was made for (another game version?)"); goto out; }
    if (legal) {
        uint32_t pw = pow_last(B);
        uint64_t found = 0;
        size_t at = HEAD;
        Op o;
        while (next_op(p, n, &at, &o) == 1)
            if (o.op == OP_ADD) found += carried(&x, o.data, o.len, rd, ctx, tmp, pw);
        if (found > LEGAL_MAX) {
            *why = mt_fmt("refused: %llu KiB of what it adds is the game's own file it patches, and a patch may carry only the change",
                          (unsigned long long)(found / 1024));
            if (refused) *refused = 1;
            goto out;
        }
    }
    part = mt_fmt("%s.part", out_path), dir = parent_of(out_path);
    mt_mkdirs(dir);
    if (!(f = mt_fopen(part, "wb"))) { *why = mt_fmt("cannot write %s", part); goto out; }
    {
        MtSha256 s;
        size_t at = HEAD;
        Op o;
        int bad = 0;
        mt_sha256_init(&s);
        while (!bad && next_op(p, n, &at, &o) == 1) {
            if (o.op == OP_ADD) {
                bad |= fwrite(o.data, 1, (size_t)o.len, f) != o.len;
                mt_sha256_update(&s, o.data, (size_t)o.len);
                continue;
            }
            for (uint64_t done = 0; !bad && done < o.len;) {
                uint64_t k = o.len - done < CHUNK ? o.len - done : CHUNK;
                if (o.op == OP_COPY) bad |= rd(ctx, o.off + done, buf, k) != (int64_t)k;
                else memset(buf, o.fill, (size_t)k);
                if (!bad) bad |= fwrite(buf, 1, (size_t)k, f) != k, mt_sha256_update(&s, buf, (size_t)k);
                done += k;
            }
        }
        bad |= fclose(f) != 0;
        f = NULL;
        mt_sha256_final(&s, sha);
        if (bad) { *why = mt_fmt("writing %s failed (is the disk full?)", part); mt_remove_file(part); goto out; }
        if (memcmp(sha, h.dst_sha, 32)) { *why = mt_strdup("the result is not what the patch promises (a damaged patch)"); mt_remove_file(part); goto out; }
    }
    if (mt_rename(part, out_path)) { *why = mt_fmt("cannot put %s in place", out_path); mt_remove_file(part); goto out; }
    ok = 0;
out:
    if (f) fclose(f), mt_remove_file(part);
    index_free(&x);
    free(buf), free(tmp), free(part), free(dir);
    return ok;
}

/* ---------------------------------------------------------------- making one (the modtopia tool, on the modder's own files) */
static int seek64(FILE *f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static int64_t file_read(void *ctx, uint64_t off, void *buf, uint64_t n) {
    FILE *f = ctx;
    if (seek64(f, off)) return -1;
    return (int64_t)fread(buf, 1, (size_t)n, f);
}
/* the source a byte at a time around the last one read (extending a match both ways) */
typedef struct { FILE *f; uint8_t *buf; uint64_t base, n; } Near;
static int near_byte(Near *s, uint64_t off) {
    if (off < s->base || off >= s->base + s->n) {
        s->base = off >= CHUNK / 2 ? off - CHUNK / 2 : 0;
        if (seek64(s->f, s->base)) return -1;
        s->n = fread(s->buf, 1, CHUNK, s->f);
        if (off >= s->base + s->n) return -1;
    }
    return s->buf[off - s->base];
}
/* the target, read once in order (its sha256 on the way) */
typedef struct { FILE *f; uint8_t *buf; size_t n, at; uint64_t pos; MtSha256 sha; } In;
static int in_get(In *r) {
    if (r->at == r->n) {
        r->n = fread(r->buf, 1, CHUNK, r->f), r->at = 0;
        if (!r->n) return -1;
        mt_sha256_update(&r->sha, r->buf, r->n);
    }
    r->pos++;
    return r->buf[r->at++];
}
static void op_var(MtBuf *b, uint64_t v) {
    uint8_t t[10];
    int k = 0;
    do { t[k] = v & 0x7f, v >>= 7; if (v) t[k] |= 0x80; k++; } while (v);
    mt_buf_add(b, t, (size_t)k);
}
/* new bytes: ADD, with runs of 64 or more of one byte as FILL */
static void emit_add(MtBuf *o, const uint8_t *d, size_t n, MtPatchStats *st) {
    size_t i = 0, start = 0;
    uint8_t c;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && d[j] == d[i]) j++;
        if (j - i >= 64) {
            if (i > start) c = OP_ADD, mt_buf_add(o, &c, 1), op_var(o, i - start), mt_buf_add(o, d + start, i - start), st->added += i - start, st->ops++;
            c = OP_FILL, mt_buf_add(o, &c, 1), op_var(o, j - i), mt_buf_add(o, d + i, 1), st->filled += j - i, st->ops++;
            start = j;
        }
        i = j;
    }
    if (n > start) c = OP_ADD, mt_buf_add(o, &c, 1), op_var(o, n - start), mt_buf_add(o, d + start, n - start), st->added += n - start, st->ops++;
}
static void emit_copy(MtBuf *o, uint64_t off, uint64_t len, MtPatchStats *st) {
    uint8_t c = OP_COPY;
    mt_buf_add(o, &c, 1), op_var(o, off), op_var(o, len);
    st->copied += len, st->ops++;
}
static int out_flush(FILE *f, MtBuf *o) {
    int bad = o->n && fwrite(o->p, 1, o->n, f) != o->n;
    o->n = 0;
    return bad;
}

int mt_patch_make(const char *src_path, const char *dst_path, const char *out_path, char **why, MtPatchStats *stp) {
    MtPatchStats st = {0};
    Index x = {0};
    Near near = {0};
    In in = {0};
    MtBuf o = {0};
    FILE *sf = mt_fopen(src_path, "rb"), *df = mt_fopen(dst_path, "rb"), *of = NULL;
    uint64_t ssize = 0;
    int64_t mt = 0;
    uint8_t ssha[32], head[HEAD] = {0}, *pend = NULL, *tmp = NULL;
    size_t np = 0;
    char *part = NULL, *dir = NULL;
    int ok = -1;
    *why = NULL;
    if (!sf || !df) { *why = mt_fmt("cannot read %s", !sf ? src_path : dst_path); goto out; }
    if (mt_file_info(src_path, &ssize, &mt)) { *why = mt_fmt("cannot read %s", src_path); goto out; }
    uint32_t B = mt_patch_block(ssize), pw = pow_last(B), h = 0, wl = 0;
    near.f = sf, near.buf = malloc(CHUNK), in.f = df, in.buf = malloc(CHUNK), tmp = malloc(B), pend = malloc(FLUSH + B + 2);
    if (!near.buf || !in.buf || !tmp || !pend || index_init(&x, ssize, B)) { *why = mt_strdup("out of memory"); goto out; }
    if (scan_source(ssize, file_read, sf, &x, ssha)) { *why = mt_fmt("cannot read %s", src_path); goto out; }
    mt_sha256_init(&in.sha);
    part = mt_fmt("%s.part", out_path), dir = parent_of(out_path);
    mt_mkdirs(dir);
    if (!(of = mt_fopen(part, "wb")) || fwrite(head, 1, HEAD, of) != HEAD) { *why = mt_fmt("cannot write %s", part); goto out; }
    int c = in_get(&in);
    while (c >= 0) {
        pend[np++] = (uint8_t)c;
        if (wl == B) h -= pend[np - B - 1] * pw;   /* the byte leaving the window */
        h = h * POLY + (uint8_t)c;
        if (wl < B) wl++;
        if (wl == B) {
            int64_t k = index_find(&x, h, pend + np - B, file_read, sf, tmp);
            if (k >= 0) {   /* the window is a source block: grow the match back into the new bytes, then forward through the target */
                uint64_t s = (uint64_t)k * B, len = B;
                int b;
                np -= B;
                while (np && s && (b = near_byte(&near, s - 1)) >= 0 && b == pend[np - 1]) s--, len++, np--;
                c = in_get(&in);
                while (c >= 0 && s + len < ssize && near_byte(&near, s + len) == c) len++, c = in_get(&in);
                emit_add(&o, pend, np, &st), np = 0;
                emit_copy(&o, s, len, &st);
                wl = 0, h = 0;
                if (o.n > FLUSH && out_flush(of, &o)) goto werr;
                continue;
            }
        }
        if (np > FLUSH) {   /* keep the window's bytes for the next match to grow back into */
            emit_add(&o, pend, np - B, &st);
            memmove(pend, pend + np - B, B), np = B;
            if (out_flush(of, &o)) goto werr;
        }
        c = in_get(&in);
    }
    emit_add(&o, pend, np, &st);
    {
        uint8_t e = OP_END;
        mt_buf_add(&o, &e, 1);
    }
    if (out_flush(of, &o)) goto werr;
    memcpy(head, "MTPATCH1", 8);
    wr64(head + 8, ssize), wr64(head + 16, in.pos);
    memcpy(head + 24, ssha, 32);
    mt_sha256_final(&in.sha, head + 56);
    wr32(head + 88, B);
    if (seek64(of, 0) || fwrite(head, 1, HEAD, of) != HEAD) goto werr;
    if (fclose(of)) { of = NULL; goto werr; }
    of = NULL;
    mt_file_info(part, &st.size, &mt);
    if (mt_rename(part, out_path)) { *why = mt_fmt("cannot put %s in place", out_path); goto out; }
    if (stp) *stp = st;
    ok = 0;
    goto out;
werr:
    *why = mt_fmt("writing %s failed (is the disk full?)", part);
out:
    if (of) fclose(of);
    if (ok && part) mt_remove_file(part);
    if (sf) fclose(sf);
    if (df) fclose(df);
    index_free(&x);
    free(near.buf), free(in.buf), free(tmp), free(pend), free(o.p), free(part), free(dir);
    return ok;
}
