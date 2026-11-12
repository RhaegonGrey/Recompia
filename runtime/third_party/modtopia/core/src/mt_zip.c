/* Modtopia core: SHA-256 (FIPS 180-4), CRC-32, DEFLATE decoding (RFC 1951) and reading .pkmod / .zip packages. MIT. */
#include "mt_internal.h"

/* ---------------------------------------------------------------- SHA-256 */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}
void mt_sha256_init(MtSha256 *s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->nbuf = 0;
}
void mt_sha256_update(MtSha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->nbuf < n ? 64 - s->nbuf : n;
        memcpy(s->buf + s->nbuf, p, k);
        s->nbuf += k, p += k, n -= k;
        if (s->nbuf == 64) { sha_block(s->h, s->buf); s->nbuf = 0; }
    }
}
void mt_sha256_final(MtSha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0;
    mt_sha256_update(s, &pad, 1);
    while (s->nbuf != 56) mt_sha256_update(s, &zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    mt_sha256_update(s, l, 8);
    for (int i = 0; i < 8; i++) out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16), out[4 * i + 2] = (uint8_t)(s->h[i] >> 8), out[4 * i + 3] = (uint8_t)s->h[i];
}
void mt_sha256(const void *data, size_t n, uint8_t out[32]) {
    MtSha256 s;
    mt_sha256_init(&s);
    mt_sha256_update(&s, data, n);
    mt_sha256_final(&s, out);
}
void mt_sha256_hex(const void *data, size_t n, char out[65]) {
    uint8_t d[32];
    mt_sha256(data, n, d);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* ---------------------------------------------------------------- CRC-32 (the zip polynomial) */
uint32_t mt_crc32(uint32_t crc, const void *data, size_t n) {
    static uint32_t t[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
            t[i] = c;
        }
        ready = 1;
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) crc = t[(crc ^ *p++) & 0xff] ^ crc >> 8;
    return ~crc;
}

/* ---------------------------------------------------------------- DEFLATE */
typedef struct {
    const uint8_t *in;
    size_t inn, pos;
    uint32_t bits;
    int nbits;
    uint8_t *out;
    size_t outn, o;
    int bad;
} Inf;
static int getbits(Inf *s, int n) {   /* LSB first */
    while (s->nbits < n) {
        if (s->pos >= s->inn) { s->bad = 1; return 0; }
        s->bits |= (uint32_t)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
    int v = (int)(s->bits & ((1u << n) - 1));
    s->bits >>= n;
    s->nbits -= n;
    return v;
}
/* A canonical Huffman code: how many codes of each length, and the symbols in code order */
typedef struct { short count[16]; short sym[320]; } Huff;
static int huff_build(Huff *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;   /* no codes (a distance code may be empty) */
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return -1;   /* over-subscribed */
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return 0;
}
static int decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= getbits(s, 1);
        if (s->bad) return -1;
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    s->bad = 1;
    return -1;
}
static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static int codes(Inf *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->o >= s->outn) return -1;
            s->out[s->o++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = (size_t)(LBASE[sym] + getbits(s, LEXT[sym]));
            int d = decode(s, dist);
            if (d < 0 || d >= 30) return -1;
            size_t dist_ = (size_t)(DBASE[d] + getbits(s, DEXT[d]));
            if (s->bad || dist_ > s->o || s->o + len > s->outn) return -1;
            for (size_t i = 0; i < len; i++, s->o++) s->out[s->o] = s->out[s->o - dist_];
        }
    }
}
int mt_inflate(const uint8_t *in, size_t inn, uint8_t *out, size_t outn) {
    Inf s = {in, inn, 0, 0, 0, out, outn, 0, 0};
    Huff lit, dist;
    int last;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.bad) return -1;
        if (type == 0) {   /* stored */
            s.bits = 0, s.nbits = 0;
            if (s.pos + 4 > s.inn) return -1;
            unsigned len = in[s.pos] | in[s.pos + 1] << 8, nlen = in[s.pos + 2] | in[s.pos + 3] << 8;
            s.pos += 4;
            if ((len ^ 0xffff) != nlen || s.pos + len > s.inn || s.o + len > s.outn) return -1;
            memcpy(out + s.o, in + s.pos, len);
            s.pos += len, s.o += len;
        } else if (type == 1) {   /* fixed codes */
            uint8_t l[288];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (int i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dist, l, 30);
            if (codes(&s, &lit, &dist)) return -1;
        } else if (type == 2) {   /* dynamic codes */
            int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
            if (s.bad || nlen > 286 || ndist > 30) return -1;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t l[320] = {0};
            for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(&s, 3);
            Huff lc;
            if (s.bad || huff_build(&lc, l, 19)) return -1;
            memset(l, 0, sizeof l);
            for (int i = 0; i < nlen + ndist;) {
                int sym = decode(&s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
                int rep, val = 0;
                if (sym == 16) {
                    if (!i) return -1;
                    val = l[i - 1];
                    rep = 3 + getbits(&s, 2);
                } else if (sym == 17) {
                    rep = 3 + getbits(&s, 3);
                } else {
                    rep = 11 + getbits(&s, 7);
                }
                if (s.bad || i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (uint8_t)val;
            }
            if (!l[256] || huff_build(&lit, l, nlen) || huff_build(&dist, l + nlen, ndist)) return -1;
            if (codes(&s, &lit, &dist)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return s.o == outn ? 0 : -1;
}

/* ---------------------------------------------------------------- zip */
static uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }

int mt_zip_safe_name(const char *name) {
    if (!*name || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return 0;   /* absolute, a drive, a stream */
    const char *s = name;
    while (*s) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t k = (size_t)(e - s);
        if ((k == 2 && s[0] == '.' && s[1] == '.') || (k == 1 && s[0] == '.')) return 0;
        for (const char *c = s; c < e; c++)
            if ((unsigned char)*c < 0x20 || strchr("<>\"|?*", *c)) return 0;
        s = *e ? e + 1 : e;
    }
    return 1;
}

int mt_zip_open(MtZip *z, const char *path, char *err, size_t errn) {
    memset(z, 0, sizeof *z);
    size_t n;
    uint8_t *d = (uint8_t *)mt_read_file(path, &n);
    if (!d) { snprintf(err, errn, "cannot read the file"); return -1; }
    z->data = d;
    z->size = n;
    /* the end of the central directory: the last signature within the comment's reach */
    size_t eocd = SIZE_MAX;
    for (size_t i = n >= 22 ? n - 22 : 0; n >= 22; i--) {
        if (u32(d + i) == 0x06054b50) { eocd = i; break; }
        if (!i || n - i > 22 + 65535) break;
    }
    if (eocd == SIZE_MAX) { snprintf(err, errn, "not a zip file"); mt_zip_close(z); return -1; }
    uint32_t count = u16(d + eocd + 10), cdsize = u32(d + eocd + 12), cdoff = u32(d + eocd + 16);
    if (count == 0xffff || cdoff == 0xffffffffu) { snprintf(err, errn, "a zip64 file (over 4 GB or 65,535 files): not supported"); mt_zip_close(z); return -1; }
    if ((uint64_t)cdoff + cdsize > n) { snprintf(err, errn, "a damaged zip file"); mt_zip_close(z); return -1; }
    z->e = mt_alloc(sizeof *z->e * (count ? count : 1));
    size_t p = cdoff;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > n || u32(d + p) != 0x02014b50) { snprintf(err, errn, "a damaged zip directory"); mt_zip_close(z); return -1; }
        MtZipEntry *e = &z->e[z->n];
        e->flags = u16(d + p + 8);
        e->method = u16(d + p + 10);
        e->crc = u32(d + p + 16);
        e->csize = u32(d + p + 20);
        e->usize = u32(d + p + 24);
        uint32_t nl = u16(d + p + 28), xl = u16(d + p + 30), cl = u16(d + p + 32);
        e->offset = u32(d + p + 42);
        if (p + 46 + nl > n) { snprintf(err, errn, "a damaged zip directory"); mt_zip_close(z); return -1; }
        e->name = mt_strndup((const char *)d + p + 46, nl);
        for (char *c = e->name; *c; c++)
            if (*c == '\\') *c = '/';
        e->is_dir = nl && e->name[nl - 1] == '/';
        z->n++;
        p += 46 + nl + xl + cl;
    }
    return 0;
}
void mt_zip_close(MtZip *z) {
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    free(z->data);
    memset(z, 0, sizeof *z);
}
int mt_zip_find(const MtZip *z, const char *name) {
    for (int i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return i;
    return -1;
}
uint8_t *mt_zip_read(const MtZip *z, int i, size_t *n, char *err, size_t errn) {
    const MtZipEntry *e = &z->e[i];
    if (e->flags & 1) { snprintf(err, errn, "%s is encrypted", e->name); return NULL; }
    if (e->method != 0 && e->method != 8) { snprintf(err, errn, "%s uses compression method %u (only stored and deflate)", e->name, e->method); return NULL; }
    if (e->offset + 30 > z->size || u32(z->data + e->offset) != 0x04034b50) { snprintf(err, errn, "%s: a damaged entry", e->name); return NULL; }
    uint64_t at = e->offset + 30 + u16(z->data + e->offset + 26) + u16(z->data + e->offset + 28);
    if (at + e->csize > z->size || e->usize > ((uint64_t)1 << 32)) { snprintf(err, errn, "%s: a damaged entry", e->name); return NULL; }
    uint8_t *out = mt_alloc((size_t)e->usize + 1);
    if (e->method == 0) {
        if (e->csize != e->usize) { free(out); snprintf(err, errn, "%s: a damaged entry", e->name); return NULL; }
        memcpy(out, z->data + at, (size_t)e->usize);
    } else if (mt_inflate(z->data + at, (size_t)e->csize, out, (size_t)e->usize)) {
        free(out);
        snprintf(err, errn, "%s: damaged compressed data", e->name);
        return NULL;
    }
    if (mt_crc32(0, out, (size_t)e->usize) != e->crc) { free(out); snprintf(err, errn, "%s: the checksum does not match", e->name); return NULL; }
    if (n) *n = (size_t)e->usize;
    return out;
}
int mt_zip_extract(const MtZip *z, const char *prefix, const char *dir, char *err, size_t errn) {
    size_t pl = strlen(prefix);
    if (mt_mkdirs(dir)) { snprintf(err, errn, "cannot make the folder %s", dir); return -1; }
    for (int i = 0; i < z->n; i++) {
        const MtZipEntry *e = &z->e[i];
        if (strncmp(e->name, prefix, pl) || !e->name[pl]) continue;
        const char *rel = e->name + pl;
        if (!mt_zip_safe_name(rel)) { snprintf(err, errn, "the package has an unsafe file name: %s", e->name); return -1; }
        char *out = mt_path_join(dir, rel);
        if (e->is_dir) {
            mt_mkdirs(out);
            free(out);
            continue;
        }
        char *slash = strrchr(out, '/'), *bslash = strrchr(out, '\\');
        char *cut = slash > bslash ? slash : bslash;
        if (cut) { *cut = 0; mt_mkdirs(out); *cut = slash > bslash ? '/' : '\\'; }
        size_t n;
        uint8_t *data = mt_zip_read(z, i, &n, err, errn);
        int bad = !data || mt_write_file(out, data, n);
        if (data && bad) snprintf(err, errn, "cannot write %s", out);
        free(data);
        free(out);
        if (bad) return -1;
    }
    return 0;
}
