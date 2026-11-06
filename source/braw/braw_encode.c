/*
 * braw_encode.c: dng2braw's Blackmagic RAW slice encoder. Part of dng2braw (fpSup community).
 * LGPL-2.1-or-later. NOT Blackmagic software: the model below is our own, from black-box decoding of our own
 * recordings (docs/FINDINGS.md 21-30). The entropy coder uses the JPEG Annex K luminance tables (ITU-T T.81,
 * a public standard) with the BRAW changes we measured. Reference implementation: tools/braw_encode_proto.py.
 */
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
static double pnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#include "braw_encode.h"

/* ---- tiny parallel-for (rows, blocks, slices are independent) ---------------------------- */
typedef struct { void (*fn)(void *, int); void *ctx; int n; int next; pthread_mutex_t mu; } ParJob;
static void *par_worker(void *a) {
    ParJob *j = a;
    for (;;) { pthread_mutex_lock(&j->mu); int i = j->next++; pthread_mutex_unlock(&j->mu); if (i >= j->n) break; j->fn(j->ctx, i); }
    return NULL;
}
static int g_threads;
int braw_threads(void) {
    if (!g_threads) { long n = sysconf(_SC_NPROCESSORS_ONLN); const char *e = getenv("DNG2BRAW_THREADS"); if (e) n = atol(e); g_threads = n < 1 ? 1 : n > 32 ? 32 : (int)n; }
    return g_threads;
}
#ifdef __APPLE__
#include <dispatch/dispatch.h>
typedef struct { void (*fn)(void *, int); void *ctx; } GcdCtx;
static void gcd_fn(void *c, size_t i) { GcdCtx *g = c; g->fn(g->ctx, (int)i); }
#endif
void braw_par_for(int n, void (*fn)(void *, int), void *ctx) {
#ifdef __APPLE__
    /* macOS: Grand Central Dispatch's pool at user-initiated QoS (P-cores first), no thread creation per call */
    if (!getenv("DNG2BRAW_THREADS") && n > 1) {
        GcdCtx g = { fn, ctx };
        dispatch_apply_f((size_t)n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &g, gcd_fn);
        return;
    }
#endif
    int nt = braw_threads(); if (nt > n) nt = n;
    if (nt <= 1) { for (int i = 0; i < n; i++) fn(ctx, i); return; }
    ParJob j = { fn, ctx, n, 0, PTHREAD_MUTEX_INITIALIZER }; pthread_t th[32];
    for (int t = 0; t < nt; t++) pthread_create(&th[t], NULL, par_worker, &j);
    for (int t = 0; t < nt; t++) pthread_join(th[t], NULL);
}

const uint8_t braw_hdr_5to1[0x24 - 0x08] = {
    0x02, 0x08, 0x05, 0x1f, 0x0f, 0x10, 0x08, 0x80, 0x00, 0x48, 0x00, 0x04,       /* 0x08-0x13 (0x0c: 3856 x 2176) */
    0x00, 0x10, 0x01, 0x0e, 0x02, 0x0c, 0x03, 0x0a, 0x01, 0xf9, 0x01, 0x43,       /* 0x14-0x1f */
    0x05, 0x00, 0x01, 0x00 };                                                     /* 0x20-0x23 */

/* ---- forward transform (FINDINGS 22) ------------------------------------------------------- */
static double Tf(double v) { return 55.94 * sqrt(v > 0 ? v : 0) + 71.2; }
static int site(int y, int x) { return (y & 1) == 0 && (x & 1) == 0 ? 0 : (y & 1) && (x & 1) ? 2 : 1; }   /* R G B */
static uint16_t clipu16(double v) { v = nearbyint(v); return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }
static int16_t clipi16(double v) { v = nearbyint(v); return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

typedef struct { const uint16_t *cfa; int W, H; double black, *T, *H3, *H2, *s, *t[3]; BrawPlanes *P; int c; } PlCtx;
/* per-channel raw gains (R, G, B) on (photosite - black) before T: calibrates our planes to the Video Assist's HDMI raw
   (FINDINGS 38: exposure-independent offsets measured on four paired takes) */
double braw_raw_gain[3] = {1.0300, 1.0153, 1.0200};   /* fitted on 4 paired takes, -4..+2 EV (FINDINGS 38) */
static void pl_T(void *v, int y) { PlCtx *p = v; size_t i0 = (size_t)y * p->W;
    double g0 = braw_raw_gain[0], g1 = braw_raw_gain[1], g2 = braw_raw_gain[2];
    for (int x = 0; x < p->W; x++) { int c = site(y, x); double g = c == 0 ? g0 : c == 1 ? g1 : g2; p->T[i0 + x] = Tf(((double)p->cfa[i0 + x] - p->black) * g); } }
/* per-row partial sums for the interior: H3[x] = T[x-2]+T[x]+T[x+2] (same column parity), H2[x] = T[x-1]+T[x+1] */
static void pl_H(void *v, int y) { PlCtx *p = v; int W = p->W; const double *t = p->T + (size_t)y * W; double *h3 = p->H3 + (size_t)y * W, *h2 = p->H2 + (size_t)y * W;
    for (int x = 2; x < W - 2; x++) { h3[x] = t[x - 2] + t[x] + t[x + 2]; h2[x] = t[x - 1] + t[x + 1]; } }
/* one 25-tap pass: same-colour 5x5 averages of T for all three colours (edge-replicated window, as numpy 'edge'),
   then Y = T - T_own + L (FINDINGS 22) */
static void pl_Y(void *v, int y) { PlCtx *p = v; int W = p->W, H = p->H; size_t i0 = (size_t)y * W;
    int yy[5]; for (int d = 0; d < 5; d++) { int t = y + d - 2; yy[d] = t < 0 ? 0 : t >= H ? H - 1 : t; }
    int interior = y >= 2 && y < H - 2;
    for (int x = 0; x < W; x++) {
        if (interior && x >= 2 && x < W - 2) {           /* fast path: separable parity sums (FINDINGS 34) */
            size_t o = (size_t)x; const double *h3 = p->H3, *h2 = p->H2; size_t r0 = (size_t)(y - 2) * W, r1 = r0 + W, r2 = r1 + W, r3 = r2 + W, r4 = r3 + W;
            double s33 = h3[r0 + o] + h3[r2 + o] + h3[r4 + o], s32 = h2[r0 + o] + h2[r2 + o] + h2[r4 + o];   /* rows of y's parity */
            double s23 = h3[r1 + o] + h3[r3 + o], s22 = h2[r1 + o] + h2[r3 + o];                              /* the other rows */
            int py = y & 1, px = x & 1; double a[2][2];
            a[py][px] = s33; a[py][px ^ 1] = s32; a[py ^ 1][px] = s23; a[py ^ 1][px ^ 1] = s22;
            /* counts: same-parity rows 3, other 2; same-parity columns 3, other 2 -> reciprocals per (py, px) case */
            static const double inv[2][2][3] = {   /* [py][px] -> 1/count for R (0,0), G, B (1,1) */
                {{1.0 / 9, 1.0 / 12, 1.0 / 4}, {1.0 / 6, 1.0 / 13, 1.0 / 6}},
                {{1.0 / 6, 1.0 / 13, 1.0 / 6}, {1.0 / 4, 1.0 / 12, 1.0 / 9}} };
            const double *iv = inv[py][px];
            double t0 = a[0][0] * iv[0], t1 = (a[0][1] + a[1][0]) * iv[1], t2 = a[1][1] * iv[2];
            double L = 0.29 * t0 + 0.571 * t1 + 0.293 * t2 - 12.7, own = py == 0 ? (px == 0 ? t0 : t1) : (px ? t2 : t1);
            double yv = p->T[i0 + x] - own + L; p->P->Y[i0 + x] = clipu16(yv > 4095 ? 4095 : yv);
            continue;
        }
        double acc[3] = {0, 0, 0}; int cnt[3] = {0, 0, 0};
        for (int dy = 0; dy < 5; dy++) {
            const double *row = p->T + (size_t)yy[dy] * W; int ry = yy[dy] & 1;
            for (int dx = -2; dx <= 2; dx++) {
                int xx = x + dx < 0 ? 0 : x + dx >= W ? W - 1 : x + dx;
                int c = ry == 0 ? ((xx & 1) == 0 ? 0 : 1) : ((xx & 1) ? 2 : 1);
                acc[c] += row[xx]; cnt[c]++;
            }
        }
        double t0 = acc[0] / cnt[0], t1 = acc[1] / cnt[1], t2 = acc[2] / cnt[2], tc[3] = {t0, t1, t2};
        double L = 0.29 * t0 + 0.571 * t1 + 0.293 * t2 - 12.7;
        double yv = p->T[i0 + x] - tc[site(y, x)] + L; p->P->Y[i0 + x] = clipu16(yv > 4095 ? 4095 : yv);   /* the decoder clips at 12 bits */
    } }
static void pl_C(void *v, int y) { PlCtx *p = v; int W = p->W;
    for (int x = 0; x < W / 2; x++) { size_t a = (size_t)(2 * y) * W + 2 * x;                         /* per 2x2 quad */
        double R = p->T[a], G = (p->T[a + 1] + p->T[a + W]) / 2, B = p->T[a + W + 1];
        p->P->Cb[(size_t)y * (W / 2) + x] = clipi16(0.563 * (B - G)); p->P->Cr[(size_t)y * (W / 2) + x] = clipi16(0.574 * (R - G)); } }

/* Buffers persist across frames (P is reused when W, H match): allocating and first-touching ~250 MB per frame
   cost more than the arithmetic. */
static double *g_T, *g_H3, *g_H2; static size_t g_Tn;
int braw_planes_from_cfa(const uint16_t *cfa, int W, int H, double black, BrawPlanes *P) {
    size_t n = (size_t)W * H;
    if (!P->Y || P->W != W || P->H != H) {
        braw_planes_free(P); P->W = W; P->H = H;
        P->Y = malloc(n * 2); P->Cb = malloc(n / 4 * 2); P->Cr = malloc(n / 4 * 2);
    }
    if (g_Tn < n) { free(g_T); free(g_H3); free(g_H2); g_T = malloc(n * 8); g_H3 = malloc(n * 8); g_H2 = malloc(n * 8); g_Tn = g_T && g_H3 && g_H2 ? n : 0; }
    if (!P->Y || !P->Cb || !P->Cr || !g_Tn) return -1;
    PlCtx c = { cfa, W, H, black, g_T, g_H3, g_H2, NULL, {NULL, NULL, NULL}, P, 0 };
    double t0 = getenv("DNG2BRAW_PROF") ? pnow() : 0;
    braw_par_for(H, pl_T, &c); braw_par_for(H, pl_H, &c); double t1 = t0 ? pnow() : 0;
    braw_par_for(H, pl_Y, &c); double t2 = t0 ? pnow() : 0;
    braw_par_for(H / 2, pl_C, &c);
    if (t0) fprintf(stderr, "planes: T %.1f ms, Y %.1f ms, C %.1f ms\n", 1e3 * (t1 - t0), 1e3 * (t2 - t1), 1e3 * (pnow() - t2));
    return 0;
}

/* ---- luma pre-filter (FINDINGS 30) --------------------------------------------------------- */
/* decoder edge filter at strength v (0-3): l0' = (v l1 + (16-2v) l0 + v r0)/16, likewise r0' (FINDINGS 32).
   Exact inverse: with m = 16-2v, A = 16 l0' - v l1, B = 16 r0' - v r1: l0 = (mA - vB)/(m^2 - v^2), r0 = (mB - vA)/(...). */
static int g_edge = 2;                         /* default: best measured (FINDINGS 36) */
void braw_set_edge_strength(int v) { g_edge = v < 0 ? 0 : v > 3 ? 3 : v; }
int braw_edge_strength(void) { return g_edge; }
static void unpost1(double *x, int n, ptrdiff_t st) {   /* one line of n samples, stride st */
    const double v = g_edge, m = 16 - 2 * v, det = m * m - v * v;
    if (g_edge == 0) return;
    for (int b = 8; b + 1 < n && b - 2 >= 0; b += 8) {
        double l1 = x[(b - 2) * st], l0 = x[(b - 1) * st], r0 = x[b * st], r1 = x[(b + 1) * st];
        double A = 16 * l0 - v * l1, Bv = 16 * r0 - v * r1;
        x[(b - 1) * st] = (m * A - v * Bv) / det; x[b * st] = (m * Bv - v * A) / det;
    }
}
static void pf_row(void *v, int y) { BrawPlanes *P = v; unpost1(P->Ypre + (size_t)y * P->W, P->W, 1); }
static void pf_cols(void *v, int j) { BrawPlanes *P = v; for (int x = j * 64; x < j * 64 + 64 && x < P->W; x++) unpost1(P->Ypre + x, P->H, P->W); }
int braw_prefilter(BrawPlanes *P) {
    int W = P->W, H = P->H; size_t n = (size_t)W * H;
    if (!P->Ypre && !(P->Ypre = malloc(n * 8))) return -1;
    for (size_t i = 0; i < n; i++) P->Ypre[i] = P->Y[i];
    braw_par_for(H, pf_row, P);                  /* horizontal (axis 1) first, as the prototype */
    braw_par_for((W + 63) / 64, pf_cols, P);     /* then vertical, 64 columns per job */
    return 0;
}
void braw_planes_free(BrawPlanes *P) { free(P->Y); free(P->Cb); free(P->Cr); free(P->Ypre); memset(P, 0, sizeof *P); }

/* ---- entropy coding (FINDINGS 24-29) ------------------------------------------------------- */
typedef struct { uint32_t code; int len; } Hc;
static Hc DC[12], AC[256]; static int g_init;
static const uint8_t K5_BITS[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};     /* T.81 Table K.5 */
static const uint8_t K5_VAL[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };
static Hc hc(const char *s) { Hc h = {0, 0}; for (; *s; s++) { h.code = h.code << 1 | (*s == '1'); h.len++; } return h; }
static void init_tables(void) {
    if (g_init) return;
    uint32_t code = 0; int k = 0;
    for (int L = 1; L <= 16; L++) { for (int i = 0; i < K5_BITS[L - 1]; i++, k++) { AC[K5_VAL[k]].code = code++; AC[K5_VAL[k]].len = L; } code <<= 1; }
    /* BRAW changes (FINDINGS 29): 0/9, 0/A move to 12 bits; the symbols they displace move */
    AC[0x09] = hc("111111110100"); AC[0x0a] = hc("111111110101"); AC[0x15] = hc("111111101100");
    AC[0x24] = hc("1111111011100"); AC[0x33] = hc("1111111011101"); AC[0x52] = hc("1111111011110"); AC[0x72] = hc("1111111011111");
    static const char *dc[12] = {"00", "010", "011", "100", "101", "110", "11100", "11101", "111110",
                                 "111111110100", "111111110101", "111111110111"};       /* FINDINGS 26, 28 */
    for (int i = 0; i < 12; i++) DC[i] = hc(dc[i]);
    g_init = 1;
}
/* MSB-first bit writer; b == NULL counts bits only (rate-control passes) */
typedef struct { Buf *b; uint64_t acc; int n; size_t bits; } Bw;
static inline void put(Bw *w, uint32_t v, int len) {      /* len <= 24; flushes 32 bits at a time */
    w->bits += len;
    if (!w->b) return;
    w->acc = (w->acc << len) | (v & ((1u << len) - 1)); w->n += len;
    if (w->n >= 32) {
        Buf *b = w->b;
        if (b->n + 4 > b->cap) { size_t n0 = b->n; bzero_(b, 4096); b->n = n0; }   /* grow via the Buf helpers */
        w->n -= 32; uint32_t u = (uint32_t)(w->acc >> w->n);
        uint8_t *d = b->p + b->n; d[0] = u >> 24; d[1] = u >> 16; d[2] = u >> 8; d[3] = u; b->n += 4;
    }
}
static inline int size_of(int v) { unsigned a = (unsigned)(v < 0 ? -v : v); return a ? 32 - __builtin_clz(a) : 0; }   /* JPEG category */
static void put_val(Bw *w, int v, int s) { if (s) put(w, (uint32_t)(v > 0 ? v : v + (1 << s) - 1), s); }
static void put_dc(Bw *w, int d) { if (d > 2047) d = 2047; if (d < -2047) d = -2047; int s = size_of(d); put(w, DC[s].code, DC[s].len); put_val(w, d, s); }
static void put_ac(Bw *w, const int *c, int n) {      /* c[0..n-1]: scan positions 1..n */
    int last = n - 1; while (last >= 0 && !c[last]) last--;
    int run = 0;
    for (int i = 0; i <= last; i++) {
        int v = c[i]; if (!v) { run++; continue; }
        while (run > 15) { put(w, AC[0xf0].code, AC[0xf0].len); run -= 16; }
        if (v > 1023) v = 1023;
        if (v < -1023) v = -1023;
        int s = size_of(v), sym = run << 4 | s; put(w, AC[sym].code, AC[sym].len); put_val(w, v, s); run = 0;
    }
    put(w, AC[0].code, AC[0].len);
}

/* ---- transform + quantiser (FINDINGS 28, 30) ------------------------------------------------ */
#define YSTEP (99.0 / 8)
#define CSTEP (99.0 / 16)
#define YDC (99.0 / 64)
#define YOFF 2047.92
#define CDC (35.0 / 32)
static double C8[8][8], C4[4][4]; static int ZZ8[64][2];
static void init_dct(void) {
    static int done; if (done) return;
    for (int k = 0; k < 8; k++) for (int x = 0; x < 8; x++) C8[k][x] = sqrt((k ? 2.0 : 1.0) / 8) * cos(M_PI * (2 * x + 1) * k / 16);
    for (int k = 0; k < 4; k++) for (int x = 0; x < 4; x++) C4[k][x] = sqrt((k ? 2.0 : 1.0) / 4) * cos(M_PI * (2 * x + 1) * k / 8);
    /* luma: JPEG zigzag over 8x8 */
    int i = 0; for (int s = 0; s < 15; s++) for (int t = 0; t <= s; t++) { int v = s & 1 ? t : s - t, u = s - v; if (v < 8 && u < 8) { ZZ8[i][0] = v; ZZ8[i][1] = u; i++; } }
    done = 1;
}
/* chroma scan (8 wide x 4 tall), FINDINGS 25: taken from the prototype's table */
static const uint8_t ZZC_TAB[32][2] = {
#include "braw_zzc.inc"
};
static void dct8x8(const double *b, double D[8][8]) {     /* D = C8 b C8^T, b row-major 8x8 */
    double t[8][8];
    for (int k = 0; k < 8; k++) for (int x = 0; x < 8; x++) { double s = 0; for (int y = 0; y < 8; y++) s += C8[k][y] * b[y * 8 + x]; t[k][x] = s; }
    for (int k = 0; k < 8; k++) for (int l = 0; l < 8; l++) { double s = 0; for (int x = 0; x < 8; x++) s += t[k][x] * C8[l][x]; D[k][l] = s; }
}
static void dct4x8(const double *b, double D[4][8]) {
    double t[4][8];
    for (int k = 0; k < 4; k++) for (int x = 0; x < 8; x++) { double s = 0; for (int y = 0; y < 4; y++) s += C4[k][y] * b[y * 8 + x]; t[k][x] = s; }
    for (int k = 0; k < 4; k++) for (int l = 0; l < 8; l++) { double s = 0; for (int x = 0; x < 8; x++) s += t[k][x] * C8[l][x]; D[k][l] = s; }
}
static int q(double v) { return (int)nearbyint(v); }

static double g_cfac = 1.0;                    /* chroma matrix relative to luma (FINDINGS 36) */
void braw_set_chroma_factor(double f) { g_cfac = f > 0.1 && f < 10 ? f : 1.0; }
void braw_quant_scaled(BrawQuant *Q, double scale) {
    for (int i = 0; i < 96; i++) {
        double v = (i == 0 || i == 64) ? 33 : nearbyint(33 * scale * (i >= 64 ? g_cfac : 1.0));
        Q->q[i] = (uint16_t)(v < 1 ? 1 : v > 4095 ? 4095 : v);
    }
}
/* coefficient storage: luma block (by, bx) at Y[(by*bw + bx)*64], [0] = block mean, [1..63] = zigzag AC;
   chroma block (by, bx) (4 x 8 chroma px; bx in pairs) at C[(by*bw/2 + bx)*32], [0] = mean, [1..31] = scan AC */
typedef struct { const BrawPlanes *P; BrawCoefs *K; } CoCtx;
static void co_row(void *v, int by) { CoCtx *c = v; const BrawPlanes *P = c->P; BrawCoefs *K = c->K; int W = P->W, CW = W / 2;
    for (int bx = 0; bx < K->bw; bx++) {
        double b[64], D[8][8], mean = 0; float *o = K->Y + ((size_t)by * K->bw + bx) * 64;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { double v = P->Ypre[(size_t)(by * 8 + y) * W + bx * 8 + x]; b[y * 8 + x] = v; mean += v; }
        dct8x8(b, D); o[0] = mean / 64;
        for (int i = 1; i < 64; i++) o[i] = D[ZZ8[i][0]][ZZ8[i][1]];
    }
    for (int pl = 0; pl < 2; pl++) {
        const int16_t *C = pl ? P->Cr : P->Cb;
        for (int bx = 0; bx < K->bw / 2; bx++) {
            double b[32], D[4][8], mean = 0; float *o = K->C[pl] + ((size_t)by * (K->bw / 2) + bx) * 32;
            for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) { double v = C[(size_t)(by * 4 + y) * CW + bx * 8 + x]; b[y * 8 + x] = v; mean += v; }
            dct4x8(b, D); o[0] = mean / 32;
            for (int i = 1; i < 32; i++) o[i] = D[ZZC_TAB[i][0]][ZZC_TAB[i][1]];
        }
    }
}
int braw_coefs(const BrawPlanes *P, BrawCoefs *K) {
    init_dct(); init_tables();
    int W = P->W, H = P->H;
    if (!K->Y || K->W != W || K->H != H) {                 /* reused across frames */
        braw_coefs_free(K); K->W = W; K->H = H; K->bw = W / 8; K->bh = H / 8;
        size_t nb = (size_t)K->bw * K->bh;
        K->Y = malloc(nb * 64 * sizeof(float)); K->C[0] = malloc(nb / 2 * 32 * sizeof(float)); K->C[1] = malloc(nb / 2 * 32 * sizeof(float));
    }
    if (!K->Y || !K->C[0] || !K->C[1]) return -1;
    CoCtx c = { P, K }; braw_par_for(K->bh, co_row, &c);
    return 0;
}
void braw_coefs_free(BrawCoefs *K) { if (!K->external) { free(K->Y); free(K->C[0]); free(K->C[1]); } memset(K, 0, sizeof *K); }

static size_t slice_core(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h, Buf *out);
void braw_recip_steps(const BrawQuant *Q, float ys[64], float cs[32]);
int braw_encode_slice_coefs(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h, Buf *out) { slice_core(K, Q, x0, y0, w, h, out); return 0; }
/* bytes the slice would take (64-bit padded), without writing it */
size_t braw_slice_size(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h) { return (slice_core(K, Q, x0, y0, w, h, NULL) + 63) / 64 * 8; }
static size_t slice_core(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h, Buf *out) {
    init_tables(); init_dct();
    float ys[64], cs[32];                        /* reciprocal steps per scan position, float like the GPU counter */
    braw_recip_steps(Q, ys, cs);
    int nr = h / 8, nbx = w / 8, bx0 = x0 / 8, by0 = y0 / 8;
    Bw bw = {out, 0, 0, 0};
    double py = 0; int pb = 0, pr = 0;
    for (int row = 0; row < nr; row++)
        for (int k = 0; k < nbx / 2; k++) {
            for (int col = 2 * k; col <= 2 * k + 1; col++) {
                const float *o = K->Y + ((size_t)(by0 + row) * K->bw + bx0 + col) * 64; int ac[63];
                for (int i = 1; i < 64; i++) { float x = o[i] * ys[i]; ac[i - 1] = (int)copysignf(truncf(fabsf(x) + ys[0]), x); }   /* vectorises (NEON) */
                int d = q((o[0] - YOFF) / YDC - py); if (d > 2047) d = 2047; if (d < -2047) d = -2047; py += d;
                put_dc(&bw, d); put_ac(&bw, ac, 63);
            }
            for (int pl = 0; pl < 2; pl++) {
                const float *o = K->C[pl] + ((size_t)(by0 + row) * (K->bw / 2) + bx0 / 2 + k) * 32; int ac[31];
                for (int i = 1; i < 32; i++) { float x = o[i] * cs[i]; ac[i - 1] = (int)copysignf(truncf(fabsf(x) + cs[0]), x); }
                int v = q(o[0] / CDC), *pp = pl ? &pr : &pb, d;
                if (row == 0 && k == 0) { d = v < -2047 ? -2047 : v > 2047 ? 2047 : v; *pp = d; }
                else { d = v - *pp; if (d > 2047) d = 2047; if (d < -2047) d = -2047; *pp += d; }
                put_dc(&bw, d); put_ac(&bw, ac, 31);
            }
            put(&bw, (uint32_t)(g_edge * 0x55), 8);      /* edge-filter strengths, 2 bits per edge (FINDINGS 32) */
        }
    if (out) while (bw.bits % 64) { int k = 64 - (int)(bw.bits % 64); put(&bw, 0, k > 24 ? 24 : k); }   /* pad to 64 bits */
    return bw.bits;
}

int braw_encode_slice_planes(const BrawPlanes *P, int x0, int y0, int w, int h, Buf *out) {
    static BrawCoefs K; static const BrawPlanes *last; BrawQuant Q;
    if (last != P || !K.Y) { braw_coefs_free(&K); if (braw_coefs(P, &K)) return -1; last = P; }
    braw_quant_scaled(&Q, 1.0);
    return braw_encode_slice_coefs(&K, &Q, x0, y0, w, h, out);
}

/* All slices of a frame in parallel. geometry: n slices with x0, y0, w, h each (geo[4*i..]); outs[i] get the bits. */
typedef struct { const BrawCoefs *K; const BrawQuant *Q; const int *geo; Buf *outs; } SlCtx;
static void sl_one(void *v, int i) { SlCtx *c = v; c->outs[i].n = 0; braw_encode_slice_coefs(c->K, c->Q, c->geo[4 * i], c->geo[4 * i + 1], c->geo[4 * i + 2], c->geo[4 * i + 3], &c->outs[i]); }
static void sl_count(void *v, int i) { SlCtx *c = v; c->outs[i].n = braw_slice_size(c->K, c->Q, c->geo[4 * i], c->geo[4 * i + 1], c->geo[4 * i + 2], c->geo[4 * i + 3]); }
size_t braw_encode_slices(const BrawCoefs *K, const BrawQuant *Q, int n, const int *geo, Buf *outs) {
    init_tables(); init_dct();
    SlCtx c = { K, Q, geo, outs }; braw_par_for(n, sl_one, &c);
    size_t t = 0; for (int i = 0; i < n; i++) t += outs[i].n; return t;
}
size_t braw_count_slices(const BrawCoefs *K, const BrawQuant *Q, int n, const int *geo) {
    init_tables(); init_dct();
    static Buf sizes[BRAW_MAX_SLICES]; if (n > BRAW_MAX_SLICES) return 0;
    SlCtx c = { K, Q, geo, sizes }; braw_par_for(n, sl_count, &c);
    size_t t = 0; for (int i = 0; i < n; i++) t += sizes[i].n; return t;
}

/* scan orders for other back ends (the Metal path) */
void braw_scan_tables(int zz8[64][2], int zzc[32][2]) {
    init_dct();
    for (int i = 0; i < 64; i++) { zz8[i][0] = ZZ8[i][0]; zz8[i][1] = ZZ8[i][1]; }
    for (int i = 0; i < 32; i++) { zzc[i][0] = ZZC_TAB[i][0]; zzc[i][1] = ZZC_TAB[i][1]; }
}

/* For GPU rate passes: AC code lengths by symbol, and each slice's matrix-independent bits (DC codes, the per-pair
   fields). DC values don't depend on the AC matrix (luma DC step fixed; chroma DC entry stays 33). */
void braw_ac_lengths(uint8_t len[256]) { init_tables(); for (int i = 0; i < 256; i++) len[i] = (uint8_t)AC[i].len; }
size_t braw_slice_fixed_bits(const BrawCoefs *K, int x0, int y0, int w, int h) {
    init_tables();
    int nr = h / 8, nbx = w / 8, bx0 = x0 / 8, by0 = y0 / 8; size_t bits = 0;
    double py = 0; int pb = 0, pr = 0;
    for (int row = 0; row < nr; row++)
        for (int k = 0; k < nbx / 2; k++) {
            for (int col = 2 * k; col <= 2 * k + 1; col++) {
                const float *o = K->Y + ((size_t)(by0 + row) * K->bw + bx0 + col) * 64;
                int d = q((o[0] - YOFF) / YDC - py); if (d > 2047) d = 2047; if (d < -2047) d = -2047; py += d;
                int sz = size_of(d); bits += DC[sz].len + sz;
            }
            for (int pl = 0; pl < 2; pl++) {
                const float *o = K->C[pl] + ((size_t)(by0 + row) * (K->bw / 2) + bx0 / 2 + k) * 32;
                int v = q(o[0] / CDC), *pp = pl ? &pr : &pb, d;
                if (row == 0 && k == 0) { d = v < -2047 ? -2047 : v > 2047 ? 2047 : v; *pp = d; }
                else { d = v - *pp; if (d > 2047) d = 2047; if (d < -2047) d = -2047; *pp += d; }
                int sz = size_of(d); bits += DC[sz].len + sz;
            }
            bits += 8;
        }
    return bits;
}
/* rounding offset: q = sign(x) * floor(|x| + r); r = 0.5 is ordinary rounding, smaller r = a deadzone (FINDINGS 36).
   Stored in ys[0] / cs[0] (unused scan position 0) so the GPU counter sees the same value. */
static float g_round = 0.33f;                 /* default: best measured trade-off (FINDINGS 36) */
void braw_set_rounding(double r) { g_round = (float)(r < 0.05 ? 0.05 : r > 0.5 ? 0.5 : r); }
void braw_recip_steps(const BrawQuant *Q, float ys[64], float cs[32]) {   /* per scan position, as slice_core uses */
    init_dct(); ys[0] = cs[0] = g_round;
    for (int i = 1; i < 64; i++) ys[i] = (float)(8.0 / (Q->q[ZZ8[i][0] * 8 + ZZ8[i][1]] * 3.0));
    for (int i = 1; i < 32; i++) cs[i] = (float)(16.0 / (Q->q[64 + ZZC_TAB[i][0] * 8 + ZZC_TAB[i][1]] * 3.0));
}
