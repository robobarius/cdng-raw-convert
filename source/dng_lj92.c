/*
 * dng_lj92.c: lossless JPEG (ITU T.81 process 14, "LJ92") decoder for DNG tiles/strips.
 *
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Written from the JPEG specification; handles what DNG writers use: SOF3,
 * 1-4 components, predictors 1-7, point transform, restart intervals.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define LJ_API __declspec(dllexport)
#else
#define LJ_API __attribute__((visibility("default")))
#endif

typedef struct { uint16_t code[256]; uint8_t len[256]; uint8_t sym[256]; int n;
                 int16_t lut[1 << 9]; uint8_t lutlen[1 << 9]; } Huff;

typedef struct { const uint8_t *p, *end; uint32_t acc; int n; int marker; } Bits;

static void huff_build(Huff *h, const uint8_t counts[16], const uint8_t *syms) {
    int k = 0, code = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < counts[l - 1]; i++) { h->code[k] = code++; h->len[k] = l; h->sym[k] = syms[k]; k++; }
        code <<= 1;
    }
    h->n = k;
    for (int i = 0; i < (1 << 9); i++) { h->lut[i] = -1; h->lutlen[i] = 0; }
    for (int i = 0; i < k; i++) if (h->len[i] <= 9) {
        int sh = 9 - h->len[i], base = h->code[i] << sh;
        for (int j = 0; j < (1 << sh); j++) { h->lut[base + j] = h->sym[i]; h->lutlen[base + j] = h->len[i]; }
    }
}

static void fill(Bits *b) {
    while (b->n <= 24) {
        uint32_t c = 0;
        if (!b->marker && b->p < b->end) {
            c = *b->p++;
            if (c == 0xFF) {
                uint8_t nx = b->p < b->end ? *b->p : 0;
                if (nx == 0x00) b->p++;
                else { b->marker = 1; b->p--; c = 0; }   /* leave marker in place */
            }
        }
        b->acc |= c << (24 - b->n); b->n += 8;
    }
}
static inline uint32_t peek(Bits *b, int n) { fill(b); return b->acc >> (32 - n); }
static inline void skip(Bits *b, int n) { b->acc <<= n; b->n -= n; }
static inline uint32_t getbits(Bits *b, int n) { if (!n) return 0; uint32_t v = peek(b, n); skip(b, n); return v; }

static int huff_decode(Bits *b, const Huff *h) {
    uint32_t v = peek(b, 16);
    int i9 = v >> 7;
    if (h->lutlen[i9]) { skip(b, h->lutlen[i9]); return h->lut[i9]; }
    for (int i = 0; i < h->n; i++)
        if ((int)(v >> (16 - h->len[i])) == h->code[i]) { skip(b, h->len[i]); return h->sym[i]; }
    return -1;
}
static inline int diff_decode(Bits *b, const Huff *h) {
    int s = huff_decode(b, h);
    if (s <= 0) return 0;
    if (s == 16) return -32768;
    int v = (int)getbits(b, s);
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

/*
 * Decode one LJ92 stream into out (out_w x out_h samples, row stride in samples).
 * The decoded samples are laid out row by row: one JPEG row of W*ncomp samples
 * fills consecutive output samples (wrapping to the next output row when out_w
 * is narrower), which is how DNG maps JPEG components onto CFA tiles.
 * Returns 0 on success, negative on error.
 */
LJ_API int dng_lj92_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride)
{
    const uint8_t *p = data, *end = data + len;
    Huff huff[4]; memset(huff, 0, sizeof huff);
    int prec = 0, H = 0, W = 0, nc = 0, cid[4] = {0}, ctab[4] = {0}, pred = 1, pt = 0, ri = 0;
    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return -1;
    p += 2;
    for (;;) {
        while (p < end && *p != 0xFF) p++;
        while (p < end && *p == 0xFF) p++;
        if (p >= end) return -2;
        int m = *p++;
        if (m == 0xD9) return -3;
        if (p + 2 > end) return -2;
        int L = (p[0] << 8) | p[1];
        const uint8_t *seg = p + 2, *segend = p + L;
        if (segend > end) return -2;
        if (m == 0xC3) {
            prec = seg[0]; H = (seg[1] << 8) | seg[2]; W = (seg[3] << 8) | seg[4]; nc = seg[5];
            if (nc < 1 || nc > 4) return -4;
            for (int i = 0; i < nc; i++) cid[i] = seg[6 + 3 * i];
        } else if (m == 0xC4) {
            const uint8_t *q = seg;
            while (q < segend) {
                int tc_th = *q++, th = tc_th & 3, tot = 0;
                for (int i = 0; i < 16; i++) tot += q[i];
                huff_build(&huff[th], q, q + 16);
                q += 16 + tot;
            }
        } else if (m == 0xDD) {
            ri = (seg[0] << 8) | seg[1];
        } else if (m == 0xDA) {
            int ns = seg[0];
            for (int i = 0; i < ns; i++) {
                int id = seg[1 + 2 * i], t = seg[2 + 2 * i] >> 4;
                for (int c = 0; c < nc; c++) if (cid[c] == id) ctab[c] = t;
            }
            pred = seg[1 + 2 * ns]; pt = seg[3 + 2 * ns] & 15;
            p = segend;
            break;
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            return -5;                                   /* lossy JPEG: not supported */
        }
        p = segend;
    }
    if (!W || !H) return -6;
    int rowlen = W * nc;
    int *prev = (int *)calloc((size_t)rowlen, sizeof(int)), *cur = (int *)calloc((size_t)rowlen, sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return -7; }
    Bits b = { p, end, 0, 0, 0 };
    long total = (long)out_w * out_h, pos = 0;
    int initial = 1 << (prec - pt - 1);
    int mcu_count = 0, restart_row = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (ri && mcu_count == ri) {               /* restart marker: re-sync */
                b.n = 0; b.acc = 0; b.marker = 0;
                while (b.p + 1 < end && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) b.p++;
                b.p += 2; mcu_count = 0; restart_row = 1;
            }
            for (int c = 0; c < nc; c++) {
                int i = x * nc + c, pv;
                if (restart_row) {                   /* first line (of the scan or after a restart) */
                    pv = (x == 0) ? initial : cur[i - nc];
                } else if (x == 0) {
                    pv = prev[i];
                } else {
                    int ra = cur[i - nc], rb = prev[i], rc = prev[i - nc];
                    switch (pred) {
                        case 1: pv = ra; break;
                        case 2: pv = rb; break;
                        case 3: pv = rc; break;
                        case 4: pv = ra + rb - rc; break;
                        case 5: pv = ra + ((rb - rc) >> 1); break;
                        case 6: pv = rb + ((ra - rc) >> 1); break;
                        case 7: pv = (ra + rb) >> 1; break;
                        default: pv = ra;
                    }
                }
                int v = (pv + diff_decode(&b, &huff[ctab[c]])) & 0xFFFF;
                cur[i] = v;
                if (pos < total) {
                    long oy = pos / out_w, ox = pos % out_w;
                    out[oy * stride + ox] = (uint16_t)(v << pt);
                }
                pos++;
            }
            mcu_count++;
        }
        restart_row = 0;
        int *t = prev; prev = cur; cur = t;
    }
    free(prev); free(cur);
    return 0;
}
