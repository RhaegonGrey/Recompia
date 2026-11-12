/* A PNG picture as RGBA pixels, for a message with a picture (API 0.6): 8 bits a channel, grey, grey + alpha, RGB, RGBA or a palette,
   not interlaced; anything else is refused (the message then goes out without its picture). mt_png_fit brings it to a square of a
   given size by averaging, the picture's shape kept and the rest left clear. MIT. */
#include "mt_internal.h"

#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* w x h RGBA (malloc'd, 4 bytes a pixel), or NULL */
uint8_t *mt_png_rgba(const uint8_t *png, size_t n, int *w, int *h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t pal[256][4], *z = NULL, *raw = NULL, *out = NULL;
    size_t zn = 0, at = 8;
    uint32_t W = 0, H = 0;
    int type = -1, npal = 0, ok = 0;
    if (n < 8 + 25 || memcmp(png, sig, 8)) return NULL;
    memset(pal, 255, sizeof pal);
    while (at + 12 <= n) {
        uint32_t len = be32(png + at);
        const uint8_t *tag = png + at + 4, *d = png + at + 8;
        if (len > n - at - 12) goto done;
        if (!memcmp(tag, "IHDR", 4)) {
            if (len < 13) goto done;
            W = be32(d), H = be32(d + 4), type = d[9];
            if (d[8] != 8 || d[12] != 0 || !W || !H || W > 4096 || H > 4096 || (type != 0 && type != 2 && type != 3 && type != 4 && type != 6)) goto done;
        } else if (!memcmp(tag, "PLTE", 4)) {
            npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < npal; i++) memcpy(pal[i], d + 3 * i, 3);
        } else if (!memcmp(tag, "tRNS", 4) && type == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = d[i];
        } else if (!memcmp(tag, "IDAT", 4)) {
            uint8_t *bigger = realloc(z, zn + len);
            if (!bigger) goto done;
            z = bigger;
            memcpy(z + zn, d, len), zn += len;
        } else if (!memcmp(tag, "IEND", 4)) break;
        at += 12 + (size_t)len;
    }
    if (type < 0 || zn < 6 || (type == 3 && !npal)) goto done;
    {
        int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : 4;   /* bytes a pixel as stored */
        size_t line = (size_t)W * (size_t)ch, total = (line + 1) * H;
        if (!(raw = malloc(total)) || !(out = malloc((size_t)W * H * 4))) goto done;
        if (mt_inflate(z + 2, zn - 2, raw, total)) goto done;   /* past the zlib header; the checksum at the end is not read */
        for (uint32_t y = 0; y < H; y++) {   /* each line's filter undone in place */
            uint8_t *row = raw + (line + 1) * y + 1, *up = y ? row - line - 1 : NULL;
            int f = row[-1];
            if (f > 4) goto done;
            for (size_t x = 0; x < line; x++) {
                int a = x >= (size_t)ch ? row[x - ch] : 0, b = up ? up[x] : 0, c = up && x >= (size_t)ch ? up[x - ch] : 0;
                row[x] = (uint8_t)(row[x] + (f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0));
            }
            for (uint32_t x = 0; x < W; x++) {
                const uint8_t *p = row + (size_t)x * ch;
                uint8_t *o = out + ((size_t)y * W + x) * 4;
                if (type == 0) o[0] = o[1] = o[2] = p[0], o[3] = 255;
                else if (type == 2) o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255;
                else if (type == 3) memcpy(o, pal[p[0]], 4);
                else if (type == 4) o[0] = o[1] = o[2] = p[0], o[3] = p[1];
                else memcpy(o, p, 4);
            }
        }
        *w = (int)W, *h = (int)H, ok = 1;
    }
done:
    free(z), free(raw);
    if (!ok) free(out), out = NULL;
    return out;
}

/* src (w x h RGBA) into out (size x size RGBA): as large as fits with its shape kept, centred, the rest clear. Each pixel of out is the
   mean of the pixels of src under it, weighted by their alpha so clear pixels do not darken the edge */
void mt_png_fit(const uint8_t *src, int w, int h, uint8_t *out, int size) {
    int tw = w >= h ? size : (int)((int64_t)size * w / h), th = h >= w ? size : (int)((int64_t)size * h / w);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int ox = (size - tw) / 2, oy = (size - th) / 2;
    memset(out, 0, (size_t)size * size * 4);
    for (int y = 0; y < th; y++) {
        int y0 = (int)((int64_t)y * h / th), y1 = (int)((int64_t)(y + 1) * h / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((int64_t)x * w / tw), x1 = (int)((int64_t)(x + 1) * w / tw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = y0; sy < y1 && sy < h; sy++)
                for (int sx = x0; sx < x1 && sx < w; sx++) {
                    const uint8_t *p = src + ((size_t)sy * w + sx) * 4;
                    r += (uint64_t)p[0] * p[3], g += (uint64_t)p[1] * p[3], b += (uint64_t)p[2] * p[3], a += p[3], cnt++;
                }
            uint8_t *o = out + ((size_t)(y + oy) * size + x + ox) * 4;
            if (a) o[0] = (uint8_t)(r / a), o[1] = (uint8_t)(g / a), o[2] = (uint8_t)(b / a);
            o[3] = cnt ? (uint8_t)(a / cnt) : 0;
        }
    }
}
