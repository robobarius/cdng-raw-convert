/*
 * prr_core.c — experimental ProRes RAW tile encoder (proof of concept)
 *
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Bitstream layout derived from FFmpeg's open-source ProRes RAW decoder; the
 * codebook tables and scan order are from FFmpeg's libavcodec (prores_raw.c,
 * proresdata.c, (c) Paul B Mahol, Lynne and the FFmpeg developers, LGPL-2.1+).
 * This is NOT Apple's encoder and is not certified or endorsed by Apple.
 *
 * Input: a Bayer (RGGB) frame already converted to the coded domain
 *        (u = 0..65535, i.e. after the inverse linearization curve), padded to
 *        a multiple of 16 in both dimensions.
 * Output: concatenated tile payloads + a table of tile sizes.
 */
#define _USE_MATH_DEFINES          /* M_PI on MSVC */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* exported symbols (Windows DLLs need an explicit export) */
#ifdef _WIN32
#define PRR_API __declspec(dllexport)
#else
#define PRR_API __attribute__((visibility("default")))
#endif

/* count leading zeros of a non-zero 32-bit value, portable */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static int clz32(uint32_t x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
#else
static int clz32(uint32_t x) { return __builtin_clz(x); }
#endif

/* ---- codebook tables (identical to the decoder) ---- */
#define DC_CB_MAX 12
static const uint8_t dc_cb[DC_CB_MAX + 1] = {
    0x010, 0x021, 0x032, 0x033, 0x033, 0x033, 0x044, 0x044, 0x044, 0x044, 0x044, 0x044, 0x076,
};
#define AC_CB_MAX 94
static const int16_t ac_cb[AC_CB_MAX + 1] = {
    0x000, 0x211, 0x111, 0x111, 0x222, 0x222, 0x222, 0x122, 0x122, 0x122,
    0x233, 0x233, 0x233, 0x233, 0x233, 0x233, 0x233, 0x233, 0x133, 0x133,
    0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244,
    0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244, 0x244,
    0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355,
    0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355,
    0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355,
    0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x355, 0x166,
};
#define RN_CB_MAX 27
static const int16_t rn_cb[RN_CB_MAX + 1] = {
    0x200, 0x100, 0x000, 0x000, 0x211, 0x211, 0x111, 0x111, 0x011, 0x011, 0x021, 0x021, 0x222, 0x022,
    0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x022, 0x032, 0x032, 0x044
};
#define LN_CB_MAX 14
static const int16_t ln_cb[LN_CB_MAX + 1] = {
    0x100, 0x111, 0x222, 0x222, 0x122, 0x122, 0x433, 0x433, 0x233, 0x233, 0x233, 0x233, 0x233, 0x233, 0x033,
};
static const uint8_t scan[64] = {
     0,  8,  1,  9, 16, 24, 17, 25,
     2, 10,  3, 11, 18, 26, 19, 27,
    32, 40, 33, 34, 41, 48, 56, 49,
    42, 35, 43, 50, 57, 58, 51, 59,
     4, 12,  5,  6, 13, 20, 28, 21,
    14,  7, 15, 22, 29, 36, 44, 37,
    30, 23, 31, 38, 45, 52, 60, 53,
    46, 39, 47, 54, 61, 62, 55, 63
};
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TODC(x) (((x) + 1) >> 1)

/* ---- bit writer (MSB first) ---- */
typedef struct { uint8_t *buf; size_t cap, pos; uint64_t acc; int nacc; int overflow; } BW;
/* 64-bit accumulator, written out 32 bits at a time (n <= 32 per call) */
static inline void bw_put(BW *b, uint32_t v, int n) {
    if (n == 0) return;
    b->acc = (b->acc << n) | (v & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)));
    b->nacc += n;
    if (b->nacc >= 32) {
        b->nacc -= 32; uint32_t w = (uint32_t)(b->acc >> b->nacc);
        if (b->pos + 4 <= b->cap) { uint8_t *p = b->buf + b->pos; p[0] = w >> 24; p[1] = w >> 16; p[2] = w >> 8; p[3] = w; }
        else b->overflow = 1;
        b->pos += 4;
    }
}
static void bw_flush(BW *b) {
    while (b->nacc >= 8) { b->nacc -= 8; if (b->pos < b->cap) b->buf[b->pos] = (uint8_t)(b->acc >> b->nacc); else b->overflow = 1; b->pos++; }
    if (b->nacc) { if (b->pos < b->cap) b->buf[b->pos] = (uint8_t)(b->acc << (8 - b->nacc)); else b->overflow = 1; b->pos++; b->nacc = 0; }
}

/* Length in bits of the code for value v (or 99 if not representable). */
static int code_len(int cb, uint32_t v) {
    int s = cb >> 8, r = cb & 0xf, e = (cb >> 4) & 0xf;
    uint32_t lim = (uint32_t)(s + 1) << r;
    if (v < lim) return (int)(v >> r) + 1 + r;
    uint32_t N = v - lim + (1u << e);
    int L = 32 - clz32(N);
    int bits = 2 * L - e + s;
    return bits > 32 ? 99 : bits;
}
static inline void put_value(BW *b, int cb, uint32_t v) {
    int s = cb >> 8, r = cb & 0xf, e = (cb >> 4) & 0xf;
    uint32_t lim = (uint32_t)(s + 1) << r;
    if (v < lim) {                          /* q zeros, a one, then r low bits */
        int q = v >> r, n = q + 1 + r;
        uint32_t x = (1u << r) | (v & ((1u << r) - 1));
        if (n <= 32) bw_put(b, x, n); else { bw_put(b, 0, q); bw_put(b, x, 1 + r); }
    } else {                                /* escape: q zeros then N in L bits */
        uint32_t N = v - lim + (1u << e);
        int L = 32 - clz32(N);
        int q = L - e + s;
        if (q + L <= 32) bw_put(b, N, q + L); else { bw_put(b, 0, q); bw_put(b, N, L); }
    }
}

/* ---- forward DCT (orthonormal / 8 -> matches FFmpeg's 16-bit Bayer iDCT) ---- */
static double cosT[8][8];
static int cos_init = 0;
static void init_cos(void) {
    for (int k = 0; k < 8; k++)
        for (int n = 0; n < 8; n++)
            cosT[k][n] = (k == 0 ? sqrt(1.0/8) : sqrt(2.0/8)) * cos((2*n + 1) * k * M_PI / 16.0);
    cos_init = 1;
}
static void fdct8x8(const double in[64], double out[64]) {
    double tmp[64];
    for (int y = 0; y < 8; y++)            /* rows: horizontal frequency */
        for (int u = 0; u < 8; u++) {
            double s = 0; for (int x = 0; x < 8; x++) s += cosT[u][x] * in[y*8 + x];
            tmp[y*8 + u] = s;
        }
    for (int u = 0; u < 8; u++)            /* cols: vertical frequency */
        for (int v = 0; v < 8; v++) {
            double s = 0; for (int y = 0; y < 8; y++) s += cosT[v][y] * tmp[y*8 + u];
            out[v*8 + u] = s / 8.0;
        }
}

static int g_dc_clamp = 1;
PRR_API void prr_set_dc_clamp(int on) { g_dc_clamp = on; }

/* Encode one component (one Bayer site) of one tile. Returns bytes written.
   Coefficients come either from img (DCT + quantisation here) or, when pre is set, from the GPU: pre holds the
   quantised blocks of this site for the whole frame, row-major, BX blocks per row (fp_metal.h). */
static size_t encode_comp(const uint16_t *img, int stride, int x0, int y0, int comp,
                          int log2_nb_blocks, const int16_t qm[64], uint8_t *out, size_t cap,
                          long *clamped, const int16_t *pre, int BX)
{
    const int nb_blocks = 1 << log2_nb_blocks;
    const int nb_codes  = 64 * nb_blocks;
    int32_t coef[64 * 16];
    double blk[64], dct[64];
    int dy = comp >> 1, dx = comp & 1;

    if (pre) {
        const int16_t *src = pre + ((size_t)(y0 / 16) * BX + x0 / 16) * 64;
        for (int i = 0; i < nb_codes; i++) coef[i] = src[i];       /* blocks of one row are consecutive */
    } else
    for (int n = 0; n < nb_blocks; n++) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int iy = y0 + 2*y + dy, ix = x0 + 2*(x + 8*n) + dx;
                blk[y*8 + x] = (double)img[(size_t)iy * stride + ix] - 32768.0;
            }
        fdct8x8(blk, dct);
        for (int i = 0; i < 64; i++) {
            double q = dct[i] / qm[i];
            long v = lround(q);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            coef[n*64 + i] = (int32_t)v;
        }
    }

    BW b = { out, cap, 0, 0, 0, 0 };

    /* --- DC --- */
    /* Optional clamp of the first DC to -16384..16383. Only matters at tile quantiser 1,
       which Apple decodes as 2 (FINDINGS.md finding 2); dng2prr never writes 1 and
       turns this off. */
    if (g_dc_clamp) {
        if (coef[0] > 16383)  { coef[0] = 16383;  (*clamped)++; }
        if (coef[0] < -16384) { coef[0] = -16384; (*clamped)++; }
    }
    int prev_dc = coef[0];
    uint32_t code = prev_dc >= 0 ? 2u * prev_dc : (uint32_t)(-2 * prev_dc - 1);
    put_value(&b, 700, code);
    int sign = 0;
    uint32_t dc = code;
    for (int n = 1; n < nb_blocks; n++) {
        int cb = ((n & 15) == 1) ? 100 : dc_cb[MIN(TODC(dc), DC_CB_MAX)];
        int d = coef[n*64] - prev_dc;
        /* make sure it is representable; clamp otherwise (rare) */
        for (;;) {
            uint32_t m = d < 0 ? -d : d;
            if (d == 0) code = 0;
            else if (d > 0) code = sign ? 2*m - 1 : 2*m;
            else            code = sign ? 2*m : 2*m - 1;
            if (code_len(cb, code) <= 32) break;
            d = d > 0 ? d - (d >> 3) - 1 : d + ((-d) >> 3) + 1;
            (*clamped)++;
        }
        put_value(&b, cb, code);
        dc = code;
        if (d != 0) sign = d < 0; else sign = 0;
        prev_dc += d;
        coef[n*64] = prev_dc;
    }

    /* --- AC: interleaved across blocks, in scan order --- */
    #define COEF(n) coef[(((n) & (nb_blocks - 1)) << 6) + scan[(n) >> log2_nb_blocks]]
    int acb = 49, rcb = 0, lcb = 66;
    int n = nb_blocks;
    while (n < nb_codes) {
        /* run of non-zero levels */
        int ln = 0;
        while (n + ln < nb_codes && COEF(n + ln) != 0) ln++;
        /* if nothing but zeros remains, stop: the decoder treats end-of-data as zeros */
        if (ln == 0) {
            int k = n; while (k < nb_codes && COEF(k) == 0) k++;
            if (k == nb_codes) break;
        }
        put_value(&b, lcb, ln);
        for (int i = 0; i < ln; i++) {
            int v = COEF(n);
            uint32_t a = (v < 0 ? -v : v) - 1;
            put_value(&b, acb, a);
            acb = ac_cb[MIN(a, AC_CB_MAX)];
            bw_put(&b, v < 0, 1);
            n++;
        }
        if (n >= nb_codes) break;
        /* run of zeros (at least one) */
        int z = 0;
        while (n + z < nb_codes && COEF(n + z) == 0) z++;
        if (n + z >= nb_codes) break;          /* trailing zeros: implicit */
        uint32_t rn = z - 1;
        put_value(&b, rcb, rn);
        rcb = rn_cb[MIN(rn, RN_CB_MAX)];
        n += z;
        int v = COEF(n);
        uint32_t a = (v < 0 ? -v : v) - 1;
        put_value(&b, acb, a);
        acb = ac_cb[MIN(a, AC_CB_MAX)];
        lcb = ln_cb[MIN(a, LN_CB_MAX)];
        bw_put(&b, v < 0, 1);
        n++;
    }
    bw_flush(&b);
    return b.overflow ? (size_t)-1 : b.pos;
}

/*
 * Encode all tiles of a frame.
 *  img      : coded-domain samples, padded to W16 x H16 (stride in samples)
 *  w, h     : real (even) frame dimensions from the header
 *  align    : log2 of max tile width in 16-px units (0..4)
 *  scale    : per-tile quantiser (2..255; Apple decodes 1 as 2, so avoid 1)
 *  qmat     : 64-entry quant matrix (raster order)
 *  out/cap  : destination for tile payloads
 *  tile_sz  : receives size of each tile (caller provides >= nb_tiles entries)
 * Returns number of tiles, or -1 on overflow. *out_len gets payload length.
 */
static int encode_tiles(const uint16_t *img, int stride, int w, int h, int align, int scale,
                     const uint8_t *qmat, uint8_t *out, size_t cap,
                     uint16_t *tile_sz, size_t *out_len, long *clamped, const int16_t *pre)
{
    if (!cos_init) init_cos();
    int16_t qm[64];
    for (int i = 0; i < 64; i++) qm[i] = (int16_t)(qmat[i] * scale);

    int tw16 = (w + 15) >> 4, th16 = (h + 15) >> 4;
    size_t pos = 0; int nt = 0;
    *clamped = 0;
    for (int ty = 0; ty < th16; ty++) {
        int tx = 0, rem = tw16;
        for (int e = align; rem > 0; e--) {
            int unit = 1 << e;
            while (unit <= rem) {
                size_t start = pos;
                /* tile header: header_len<<3, scale, size[comp2], size[comp1], size[comp3] */
                if (pos + 8 > cap) return -1;
                uint8_t *hdr = out + pos;
                hdr[0] = 8 << 3; hdr[1] = (uint8_t)scale;
                pos += 8;
                static const int order[4] = { 2, 1, 3, 0 };
                size_t sz[4];
                for (int k = 0; k < 4; k++) {
                    int BX = (w + 15) >> 4, BY = (h + 15) >> 4;
                    size_t s = encode_comp(img, stride, tx * 16, ty * 16, order[k], e, qm,
                                           out + pos, cap - pos, clamped,
                                           pre ? pre + (size_t)order[k] * BY * BX * 64 : NULL, BX);
                    if (s == (size_t)-1) return -1;
                    sz[k] = s; pos += s;
                }
                if (sz[0] > 65535 || sz[1] > 65535 || sz[2] > 65535) return -2;
                hdr[2] = sz[0] >> 8; hdr[3] = sz[0];
                hdr[4] = sz[1] >> 8; hdr[5] = sz[1];
                hdr[6] = sz[2] >> 8; hdr[7] = sz[2];
                size_t tsz = pos - start;
                if (tsz > 65535) return -2;
                tile_sz[nt++] = (uint16_t)tsz;
                tx += unit; rem -= unit;
            }
        }
    }
    *out_len = pos;
    return nt;
}

PRR_API int prr_encode_tiles(const uint16_t *img, int stride, int w, int h, int align, int scale,
                     const uint8_t *qmat, uint8_t *out, size_t cap,
                     uint16_t *tile_sz, size_t *out_len, long *clamped)
{
    return encode_tiles(img, stride, w, h, align, scale, qmat, out, cap, tile_sz, out_len, clamped, NULL);
}
/* The same from quantised coefficients made on the GPU (fp_metal.h, fpgpu_prr): [site][by][bx][64]. */
PRR_API int prr_encode_tiles_pre(const int16_t *pre, int w, int h, int align, int scale,
                     const uint8_t *qmat, uint8_t *out, size_t cap,
                     uint16_t *tile_sz, size_t *out_len, long *clamped)
{
    return encode_tiles(NULL, 0, w, h, align, scale, qmat, out, cap, tile_sz, out_len, clamped, pre);
}
/* the quantisation matrix the tile coder uses for (qmat, scale), for the GPU */
PRR_API void prr_qm(const uint8_t *qmat, int scale, int16_t qm[64]) { for (int i = 0; i < 64; i++) qm[i] = (int16_t)(qmat[i] * scale); }
