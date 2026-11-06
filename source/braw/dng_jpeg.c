/*
 * dng_jpeg.c: JPEG codecs for compressed CinemaDNG tiles.
 *
 *   dng_lj92_encode   lossless JPEG (process 14, predictor 1), 2 components, as DNG
 *                     writers lay out CFA tiles (JPEG width = tile width / 2)
 *   dng_jpeg12_*      lossy 12-bit extended-sequential DCT JPEG (SOF1) for CFA tiles,
 *                     in the layout used by existing lossy CinemaDNG files: 2 components,
 *                     JPEG width = tile width, height = tile height / 2, so every 8x8
 *                     block holds a single CFA colour
 *   dng_jpeg_decode   decodes either kind (lossless via dng_lj92_decode, or SOF0/SOF1
 *                     DCT JPEG with 1x1 sampling) into a tile
 *
 * Samples map between the JPEG and the tile in DNG order: JPEG row by row, the
 * components of each pixel interleaved, filling tile rows consecutively.
 *
 * Part of dng2prr (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Written from ITU-T T.81; the optimal-Huffman routine follows its Annex K.2.
 */
#define _USE_MATH_DEFINES
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifdef _WIN32
#define JP_API __declspec(dllexport)
#else
#define JP_API __attribute__((visibility("default")))
#endif

int dng_lj92_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride);

static const uint8_t ZZ[64] = {          /* zig-zag index -> natural index */
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* ------------------------------------------------------------------ output buffer + bit writer */
typedef struct { uint8_t *p; size_t n, cap; uint32_t acc; int nacc; } Out;
static void o_byte(Out *o, int b) {
    if (o->n + 2 > o->cap) { o->cap = o->cap * 2 + 4096; o->p = realloc(o->p, o->cap); }
    o->p[o->n++] = (uint8_t)b;
}
static void o_u16(Out *o, int v) { o_byte(o, v >> 8); o_byte(o, v & 255); }
static void o_bits(Out *o, uint32_t v, int n) {         /* MSB first, with 0xFF byte stuffing */
    while (n > 0) {
        int take = n > 16 ? 16 : n; n -= take;
        uint32_t part = (v >> n) & ((1u << take) - 1);
        o->acc = (o->acc << take) | part; o->nacc += take;
        while (o->nacc >= 8) {
            int b = (o->acc >> (o->nacc - 8)) & 255; o->nacc -= 8;
            o_byte(o, b); if (b == 0xFF) o_byte(o, 0);
        }
    }
}
static void o_flush(Out *o) { if (o->nacc) o_bits(o, 0x7F, 8 - o->nacc); }

/* ------------------------------------------------------------------ Huffman tables */
typedef struct { uint8_t bits[17], val[257]; int nval; uint16_t code[257]; uint8_t size[257]; } HuffEnc;

/* Annex K.2: code lengths from frequencies, limited to 16 bits. freq has 257 entries
   (entry 256 is the reserved code point so that no code is all ones). */
static void huff_optimal(const long freq_in[257], HuffEnc *h) {
    long freq[257]; int codesize[257], others[257];
    memcpy(freq, freq_in, sizeof freq); freq[256] = 1;
    for (int i = 0; i < 257; i++) { codesize[i] = 0; others[i] = -1; }
    for (;;) {
        int c1 = -1, c2 = -1; long v = 1000000000L;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v) { v = freq[i]; c1 = i; }
        v = 1000000000L;
        for (int i = 0; i <= 256; i++) if (freq[i] && freq[i] <= v && i != c1) { v = freq[i]; c2 = i; }
        if (c2 < 0) break;
        freq[c1] += freq[c2]; freq[c2] = 0;
        codesize[c1]++; while (others[c1] >= 0) { c1 = others[c1]; codesize[c1]++; }
        others[c1] = c2;
        codesize[c2]++; while (others[c2] >= 0) { c2 = others[c2]; codesize[c2]++; }
    }
    int bits[33] = {0};
    for (int i = 0; i <= 256; i++) if (codesize[i]) bits[codesize[i] > 32 ? 32 : codesize[i]]++;
    for (int i = 32; i > 16; i--)
        while (bits[i] > 0) {
            int j = i - 2; while (bits[j] == 0) j--;
            bits[i] -= 2; bits[i - 1]++; bits[j + 1] += 2; bits[j]--;
        }
    int i = 16; while (bits[i] == 0) i--; bits[i]--;          /* remove the reserved code point */
    memset(h, 0, sizeof *h);
    for (int k = 1; k <= 16; k++) h->bits[k] = (uint8_t)bits[k];
    int p = 0;
    for (int len = 1; len <= 32; len++)
        for (int s = 0; s < 256; s++) if (codesize[s] == len) h->val[p++] = (uint8_t)s;
    h->nval = p;
    int code = 0, k = 0;                                       /* canonical codes */
    for (int len = 1; len <= 16; len++) {
        for (int c = 0; c < h->bits[len]; c++) { h->code[h->val[k]] = (uint16_t)code++; h->size[h->val[k]] = (uint8_t)len; k++; }
        code <<= 1;
    }
}
static void write_dht(Out *o, int tc_th, const HuffEnc *h) {
    o_byte(o, 0xFF); o_byte(o, 0xC4); o_u16(o, 2 + 1 + 16 + h->nval);
    o_byte(o, tc_th);
    for (int k = 1; k <= 16; k++) o_byte(o, h->bits[k]);
    for (int k = 0; k < h->nval; k++) o_byte(o, h->val[k]);
}
static inline int nbits(int v) { v = v < 0 ? -v : v; int n = 0; while (v) { n++; v >>= 1; } return n; }

/* ------------------------------------------------------------------ lossless (LJ92) encoder */
/* tile: th rows of tw samples (row stride `stride`), values < 2^bits. JPEG: width tw/2,
   height th, 2 components, predictor 1. */
JP_API int dng_lj92_encode(const uint16_t *tile, int tw, int th, int stride, int bits,
                           uint8_t **out_p, size_t *out_len)
{
    if (tw % 2 || bits < 2 || bits > 16) return -1;
    int W = tw / 2, nc = 2, initial = 1 << (bits - 1);
    long freq[257] = {0};
    HuffEnc h;
    memset(&h, 0, sizeof h);
    for (int pass = 0; pass < 2; pass++) {
        Out o = {0};
        if (pass == 1) {
            huff_optimal(freq, &h);
            o_byte(&o, 0xFF); o_byte(&o, 0xD8);
            o_byte(&o, 0xFF); o_byte(&o, 0xC3); o_u16(&o, 8 + 3 * nc);
            o_byte(&o, bits); o_u16(&o, th); o_u16(&o, W); o_byte(&o, nc);
            for (int c = 0; c < nc; c++) { o_byte(&o, c); o_byte(&o, 0x11); o_byte(&o, 0); }
            write_dht(&o, 0x00, &h);
            o_byte(&o, 0xFF); o_byte(&o, 0xDA); o_u16(&o, 6 + 2 * nc); o_byte(&o, nc);
            for (int c = 0; c < nc; c++) { o_byte(&o, c); o_byte(&o, 0x00); }
            o_byte(&o, 1); o_byte(&o, 0); o_byte(&o, 0);    /* predictor 1, Se 0, Ah/Al 0 */
        }
        for (int y = 0; y < th; y++) {
            const uint16_t *r = tile + (size_t)y * stride, *up = y ? r - stride : NULL;
            for (int x = 0; x < W; x++)
                for (int c = 0; c < nc; c++) {
                    int i = 2 * x + c, v = r[i];
                    int pv = (x == 0) ? (y == 0 ? initial : up[c]) : r[i - 2];
                    int d = (v - pv) & 0xFFFF; if (d >= 32768) d -= 65536;
                    int s = (d == -32768) ? 16 : nbits(d);
                    if (pass == 0) { freq[s]++; continue; }
                    o_bits(&o, h.code[s], h.size[s]);
                    if (s && s < 16) o_bits(&o, d < 0 ? (uint32_t)(d + (1 << s) - 1) : (uint32_t)d, s);
                }
        }
        if (pass == 1) {
            o_flush(&o); o_byte(&o, 0xFF); o_byte(&o, 0xD9);
            *out_p = o.p; *out_len = o.n;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ 12-bit DCT JPEG encoder */
static double COS[8][8];
static int cos_ready = 0;
static void init_cos(void) {
    for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++)
        COS[u][x] = (u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * M_PI / 16);
    cos_ready = 1;
}
static void fdct(const float in[64], float out[64]) {       /* orthonormal = JPEG's FDCT */
    double t[64];
    for (int y = 0; y < 8; y++) for (int u = 0; u < 8; u++) { double s = 0; for (int x = 0; x < 8; x++) s += COS[u][x] * in[8*y+x]; t[8*y+u] = s; }
    for (int u = 0; u < 8; u++) for (int v = 0; v < 8; v++) { double s = 0; for (int y = 0; y < 8; y++) s += COS[v][y] * t[8*y+u]; out[8*v+u] = (float)s; }
}
static void idct(const float in[64], float out[64]) {
    double t[64];
    for (int v = 0; v < 8; v++) for (int x = 0; x < 8; x++) { double s = 0; for (int u = 0; u < 8; u++) s += COS[u][x] * in[8*v+u]; t[8*v+x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) { double s = 0; for (int v = 0; v < 8; v++) s += COS[v][y] * t[8*v+x]; out[8*y+x] = (float)s; }
}

/* Forward DCT of a whole tile (codes 0..4095) in the lossy CFA layout.
   coef receives (tw/8)*(th/16)*2 blocks of 64 floats, in MCU order (block of comp 0,
   block of comp 1, ...). tw must be a multiple of 16, th of 16. */
JP_API int dng_jpeg12_dct(const uint16_t *tile, int tw, int th, int stride, float *coef)
{
    if (tw % 16 || th % 16) return -1;
    if (!cos_ready) init_cos();
    int W = tw, H = th / 2, k = 0;
    float blk[64];
    for (int by = 0; by < H / 8; by++)
        for (int bx = 0; bx < W / 8; bx++)
            for (int c = 0; c < 2; c++) {
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                    int jy = by * 8 + y, jx = bx * 8 + x;
                    long idx = 2L * ((long)jy * W + jx) + c;          /* sample index in DNG order */
                    int ty = (int)(idx / tw), tx = (int)(idx % tw);
                    blk[8*y+x] = (float)tile[(size_t)ty * stride + tx] - 2048.0f;
                }
                fdct(blk, coef + 64 * (size_t)k++);
            }
    return k;
}

/* Quantise + entropy-code DCT coefficients from dng_jpeg12_dct with quantisation table
   qt (natural order, 1..255) into a complete JPEG (SOF1, 12-bit, 2 components). */
JP_API int dng_jpeg12_encode(const float *coef, int tw, int th, const uint16_t qt[64],
                             uint8_t **out_p, size_t *out_len)
{
    int W = tw, H = th / 2, nblk = (W / 8) * (H / 8) * 2;
    long fdc[257] = {0}, fac[257] = {0};
    HuffEnc hd, ha;
    int16_t *q = malloc(sizeof(int16_t) * 64 * (size_t)nblk);
    for (int b = 0; b < nblk; b++)
        for (int i = 0; i < 64; i++) {
            int n = ZZ[i]; double v = coef[64 * (size_t)b + n] / qt[n];
            long r = lround(v); if (r > 16383) r = 16383; if (r < -16383) r = -16383;
            if (i == 0) { if (r > 16383) r = 16383; }
            q[64 * (size_t)b + i] = (int16_t)r;
        }
    for (int pass = 0; pass < 2; pass++) {
        Out o = {0};
        if (pass == 1) {
            huff_optimal(fdc, &hd); huff_optimal(fac, &ha);
            o_byte(&o, 0xFF); o_byte(&o, 0xD8);
            int q16 = 0; for (int i = 0; i < 64; i++) if (qt[i] > 255) q16 = 1;
            o_byte(&o, 0xFF); o_byte(&o, 0xDB); o_u16(&o, 3 + 64 * (q16 ? 2 : 1)); o_byte(&o, q16 ? 0x10 : 0x00);
            for (int i = 0; i < 64; i++) { if (q16) o_u16(&o, qt[ZZ[i]]); else o_byte(&o, qt[ZZ[i]]); }
            o_byte(&o, 0xFF); o_byte(&o, 0xC1); o_u16(&o, 8 + 3 * 2);
            o_byte(&o, 12); o_u16(&o, H); o_u16(&o, W); o_byte(&o, 2);
            for (int c = 0; c < 2; c++) { o_byte(&o, c); o_byte(&o, 0x11); o_byte(&o, 0); }
            write_dht(&o, 0x00, &hd); write_dht(&o, 0x10, &ha);
            o_byte(&o, 0xFF); o_byte(&o, 0xDA); o_u16(&o, 6 + 2 * 2); o_byte(&o, 2);
            for (int c = 0; c < 2; c++) { o_byte(&o, c); o_byte(&o, 0x00); }
            o_byte(&o, 0); o_byte(&o, 63); o_byte(&o, 0);
        }
        int pred[2] = {0, 0};
        for (int b = 0; b < nblk; b++) {
            const int16_t *z = q + 64 * (size_t)b; int c = b & 1;
            int d = z[0] - pred[c]; pred[c] = z[0];
            int s = nbits(d);
            if (pass == 0) fdc[s]++;
            else { o_bits(&o, hd.code[s], hd.size[s]); if (s) o_bits(&o, d < 0 ? (uint32_t)(d + (1 << s) - 1) : (uint32_t)d, s); }
            int run = 0;
            for (int i = 1; i < 64; i++) {
                int v = z[i];
                if (!v) { run++; continue; }
                while (run > 15) { if (pass == 0) fac[0xF0]++; else o_bits(&o, ha.code[0xF0], ha.size[0xF0]); run -= 16; }
                int sz = nbits(v), sym = (run << 4) | sz;
                if (pass == 0) fac[sym]++;
                else { o_bits(&o, ha.code[sym], ha.size[sym]); o_bits(&o, v < 0 ? (uint32_t)(v + (1 << sz) - 1) : (uint32_t)v, sz); }
                run = 0;
            }
            if (run) { if (pass == 0) fac[0x00]++; else o_bits(&o, ha.code[0], ha.size[0]); }
        }
        if (pass == 1) { o_flush(&o); o_byte(&o, 0xFF); o_byte(&o, 0xD9); *out_p = o.p; *out_len = o.n; }
    }
    free(q);
    return 0;
}

/* ------------------------------------------------------------------ DCT JPEG decoder (SOF0/SOF1) */
typedef struct { uint8_t bits[17], val[256]; int maxcode[18], valptr[17], mincode[17]; int16_t look[512]; uint8_t looklen[512]; } HuffDec;
static void huffdec_build(HuffDec *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code; code += h->bits[l]; k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1; code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    for (int i = 0; i < 512; i++) { h->look[i] = -1; h->looklen[i] = 0; }
    code = 0; k = 0;
    for (int l = 1; l <= 9; l++) {
        for (int c = 0; c < h->bits[l]; c++, k++, code++) {
            int sh = 9 - l; for (int j = 0; j < (1 << sh); j++) { h->look[(code << sh) + j] = h->val[k]; h->looklen[(code << sh) + j] = (uint8_t)l; }
        }
        code <<= 1;
    }
}
typedef struct { const uint8_t *p, *end; uint32_t acc; int n, marker; } BR;
static void br_fill(BR *b) {
    while (b->n <= 24) {
        uint32_t c = 0;
        if (!b->marker && b->p < b->end) {
            c = *b->p++;
            if (c == 0xFF) { uint8_t nx = b->p < b->end ? *b->p : 0; if (nx == 0) b->p++; else { b->marker = 1; b->p--; c = 0; } }
        }
        b->acc |= c << (24 - b->n); b->n += 8;
    }
}
static inline uint32_t br_get(BR *b, int n) { if (!n) return 0; br_fill(b); uint32_t v = b->acc >> (32 - n); b->acc <<= n; b->n -= n; return v; }
static int br_huff(BR *b, const HuffDec *h) {
    br_fill(b);
    int i9 = b->acc >> 23;
    if (h->looklen[i9]) { int l = h->looklen[i9]; b->acc <<= l; b->n -= l; return h->look[i9]; }
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | (int)br_get(b, 1);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l]) return h->val[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;
}
static inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

static int dct_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride)
{
    /* Baseline / extended sequential, 1x1 sampling. Handles one interleaved scan (all components) or several
       scans each carrying some of the components (SlimRAW's lossy DNGs: one scan per component). */
    const uint8_t *p = data + 2, *end = data + len;
    uint16_t qt[4][64]; HuffDec hdc[4], hac[4];
    int prec = 8, H = 0, W = 0, nc = 0, cid[4], cq[4], ri = 0, have_frame = 0, scans = 0;
    memset(qt, 0, sizeof qt);
    long total = (long)out_w * out_h;
    float blk[64], pix[64];
    for (;;) {
        while (p < end && *p != 0xFF) p++;
        while (p < end && *p == 0xFF) p++;
        if (p >= end) return scans ? 0 : -2;
        int m = *p++;
        if (m == 0xD9) return scans ? 0 : -3;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;        /* markers without a length */
        if (p + 2 > end) return scans ? 0 : -2;
        int L = (p[0] << 8) | p[1]; const uint8_t *s = p + 2, *se = p + L; if (se > end) return -2;
        if (m == 0xDB) {
            while (s < se) { int pq = *s >> 4, tq = *s & 3; s++;
                for (int i = 0; i < 64; i++) { qt[tq][ZZ[i]] = pq ? (uint16_t)((s[0] << 8) | s[1]) : s[0]; s += pq ? 2 : 1; } }
        } else if (m == 0xC0 || m == 0xC1) {
            prec = s[0]; H = (s[1] << 8) | s[2]; W = (s[3] << 8) | s[4]; nc = s[5];
            if (nc < 1 || nc > 4) return -4;
            for (int c = 0; c < nc; c++) { cid[c] = s[6 + 3*c]; if (s[7 + 3*c] != 0x11) return -8; cq[c] = s[8 + 3*c] & 3; }
            if (W % 8 || H % 8) return -6;
            have_frame = 1;
        } else if (m == 0xC4) {
            while (s < se) { int tc = *s >> 4, th = *s & 3; s++; HuffDec *h = tc ? &hac[th] : &hdc[th];
                int tot = 0; for (int l = 1; l <= 16; l++) { h->bits[l] = s[l - 1]; tot += s[l - 1]; }
                memcpy(h->val, s + 16, tot > 256 ? 256 : tot); s += 16 + tot; huffdec_build(h); }
        } else if (m == 0xDD) { ri = (s[0] << 8) | s[1];
        } else if (m == 0xDA) {
            if (!have_frame) return -7;
            int ns = s[0], sc[4], sdc[4], sac[4];
            if (ns < 1 || ns > nc) return -4;
            for (int i = 0; i < ns; i++) {
                sc[i] = -1;
                for (int c = 0; c < nc; c++) if (cid[c] == s[1 + 2*i]) sc[i] = c;
                if (sc[i] < 0) return -4;
                sdc[i] = s[2 + 2*i] >> 4; sac[i] = s[2 + 2*i] & 15;
            }
            if (!cos_ready) init_cos();
            BR b = { se, end, 0, 0, 0 };
            int pred[4] = {0}, mcus = (W / 8) * (H / 8), off = 1 << (prec - 1), maxv = (1 << prec) - 1, cnt = 0;
            for (int mcu = 0; mcu < mcus; mcu++) {
                if (ri && cnt == ri) {
                    b.n = 0; b.acc = 0; b.marker = 0;
                    while (b.p + 1 < end && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) b.p++;
                    b.p += 2; cnt = 0; memset(pred, 0, sizeof pred);
                }
                cnt++;
                int by = mcu / (W / 8), bx = mcu % (W / 8);
                for (int i = 0; i < ns; i++) {
                    int c = sc[i];
                    memset(blk, 0, sizeof blk);
                    int sz0 = br_huff(&b, &hdc[sdc[i]]);
                    pred[i] += sz0 ? extend((int)br_get(&b, sz0), sz0) : 0;
                    blk[0] = (float)(pred[i] * qt[cq[c]][0]);
                    for (int k = 1; k < 64; ) {
                        int rs = br_huff(&b, &hac[sac[i]]), r = rs >> 4, sz = rs & 15;
                        if (!sz) { if (r == 15) { k += 16; continue; } break; }
                        k += r; if (k > 63) break;
                        blk[ZZ[k]] = (float)(extend((int)br_get(&b, sz), sz) * qt[cq[c]][ZZ[k]]); k++;
                    }
                    idct(blk, pix);
                    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                        int v = (int)lrintf(pix[8*y+x]) + off; v = v < 0 ? 0 : v > maxv ? maxv : v;
                        long idx = ((long)(by * 8 + y) * W + bx * 8 + x) * nc + c;
                        if (idx < total) out[(idx / out_w) * stride + idx % out_w] = (uint16_t)v;
                    }
                }
            }
            scans++;
            /* the next marker follows the entropy-coded data: skip to it */
            const uint8_t *q = b.p;
            while (q + 1 < end && !(q[0] == 0xFF && q[1] != 0x00 && !(q[1] >= 0xD0 && q[1] <= 0xD7))) q++;
            p = q; continue;
        } else if (m == 0xC3) return -9;                  /* lossless: caller uses dng_lj92_decode */
        else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return -5;
        p = se;
    }
}

/* Decode a DNG JPEG tile of either kind. */
JP_API int dng_jpeg_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride)
{
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;
    for (size_t i = 2; i + 1 < len; ) {                  /* find the frame marker */
        if (data[i] != 0xFF) { i++; continue; }
        int m = data[i + 1];
        if (m == 0xC3) return dng_lj92_decode(data, len, out, out_w, out_h, stride);
        if (m == 0xC0 || m == 0xC1) return dct_decode(data, len, out, out_w, out_h, stride);
        if (m == 0xDA || m == 0xD9) break;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += (m == 0xFF) ? 1 : 2; continue; }
        if (i + 3 >= len) break;
        i += 2 + ((data[i + 2] << 8) | data[i + 3]);
    }
    return -5;
}
