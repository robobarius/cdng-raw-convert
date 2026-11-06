/*
 * braw_format.c: Blackmagic RAW frame layout (Video Assist 12G + SIGMA fp, as observed) and the
 * dng2braw frame builder. Part of dng2braw (fpSup community). LGPL-2.1-or-later. NOT Blackmagic software.
 * Section numbers refer to docs/FINDINGS.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "braw_format.h"
#include "braw_encode.h"
#include <pthread.h>
#ifdef BRAW_METAL
#include "braw_metal.h"
#endif

/* FINDINGS 7: 5 strips, multiples of 16, remainder in the last (768 x4 + 784 at 3856); 72-line
   slices, column-major; coded height = DNG height rounded up to 16 (2170 -> 2176). The strip
   count for other widths (DCI, open gate) is a guess until those modes are recorded. */
int braw_size_tested(int cw, int ch) {
    static const int ok[][2] = { {3856, 2176},    /* fp UHD (Video Assist HDMI raw size) */
                                 {3024, 2016} };  /* fp OG3K 3024 x 2010 (FINDINGS 40) */
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (ok[i][0] == cw && ok[i][1] == ch) return 1;
    return 0;
}
int braw_layout(int w, int h, BrawLayout *L) {
    memset(L, 0, sizeof *L);
    if (w <= 0 || h <= 0 || (w & 15)) return -1;
    L->w = w; L->h = h; L->coded_w = w; L->coded_h = (h + 15) / 16 * 16;
    L->pad_top = BRAW_PAD_TOP;
    if (L->pad_top > L->coded_h - h) L->pad_top = (L->coded_h - h) / 2;
    L->ncols = BRAW_STRIPS;
    int base = (w / L->ncols) / 16 * 16, used = 0;
    for (int i = 0; i < L->ncols - 1; i++) { L->col_w[i] = base; used += base; }
    L->col_w[L->ncols - 1] = w - used;
    L->row_h = BRAW_SLICE_LINES; L->nrows = (L->coded_h + L->row_h - 1) / L->row_h;
    if (0x80 + 4 * L->ncols * L->nrows > BRAW_HEADER_SIZE) return -1;
    return 0;
}

/* ---- splice source (developer test) ----------------------------------------------------- */
static const uint8_t *g_sp; static size_t g_splen; static int g_spn, g_spk;
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
int braw_set_splice(const uint8_t *p, size_t len, char *err, size_t errlen) {
    if (len < BRAW_HEADER_SIZE || memcmp(p, "braw", 4)) { snprintf(err, errlen, "splice: not a BRAW frame payload"); return -1; }
    int n = 0; while (0x80 + 4 * (n + 1) <= BRAW_HEADER_SIZE && rd32(p + 0x80 + 4 * n) >= BRAW_HEADER_SIZE && rd32(p + 0x80 + 4 * n) < len) n++;
    g_sp = p; g_splen = len; g_spn = n; return 0;
}
static size_t sp_end(int k) {           /* end of slice k: next slice's offset, or the payload size field */
    if (k + 1 < g_spn) return rd32(g_sp + 0x80 + 4 * (k + 1));
    size_t e = rd32(g_sp + 4); return e && e <= g_splen ? e : g_splen;
}

/* ---- GPU pipeline (FINDINGS 35): frames are prepared (CFA -> coefficients) on the GPU ahead of the CPU coder --- */
static int g_gpu_on = 1;
void braw_set_gpu(int on) { g_gpu_on = on; }
static void place_coded(const BrawLayout *L, const uint16_t *raw, uint16_t *coded) {   /* DNG rows at pad_top; same-phase padding */
    int W = L->coded_w, H = L->coded_h;
    for (int y = 0; y < H; y++) { int sy = y - L->pad_top; while (sy < 0) sy += 2; while (sy >= L->h) sy -= 2; memcpy(coded + (size_t)y * W, raw + (size_t)sy * L->w, (size_t)W * 2); }
}
#ifdef BRAW_METAL
static BrawGpu *g_gpu; static int g_gpu_state; static int g_next_slot, g_fifo[8], g_fh, g_ft;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
#endif
const char *braw_gpu_status(const BrawLayout *L) {
#ifdef BRAW_METAL
    static char msg[256];
    if (!g_gpu_on) return "off (--cpu)";
    if (!g_gpu_state) { char e[200] = ""; g_gpu = braw_gpu_create(L->coded_w, L->coded_h, e, sizeof e); g_gpu_state = g_gpu ? 1 : -1;
        snprintf(msg, sizeof msg, g_gpu ? "Metal, %s (transform + DCT on the GPU)" : "unavailable (%s); CPU path", g_gpu ? braw_gpu_name(g_gpu) : e); }
    return msg;
#else
    (void)L; return "not built (CPU path)";
#endif
}
int braw_prepare_frame(const BrawLayout *L, const uint16_t *raw, double black) {
#ifdef BRAW_METAL
    if (!g_gpu_on) return -1;
    braw_gpu_status(L); if (g_gpu_state < 0) return -1;
    int slot = g_next_slot; g_next_slot = (slot + 1) % BRAW_GPU_SLOTS;
    place_coded(L, raw, braw_gpu_cfa(g_gpu, slot));             /* straight into unified memory */
    braw_gpu_submit(g_gpu, slot, black);
    pthread_mutex_lock(&g_fmu); g_fifo[g_ft++ % 8] = slot; pthread_mutex_unlock(&g_fmu);
    return 0;
#else
    (void)L; (void)raw; (void)black; return -1;
#endif
}
static int g_cur_slot = -1;                                    /* GPU slot of the frame being built, -1 = CPU */
static int take_prepared(BrawCoefs *K) {                       /* oldest prepared frame's coefficients, or -1 */
    g_cur_slot = -1;
#ifdef BRAW_METAL
    int slot = -1; pthread_mutex_lock(&g_fmu); if (g_fh < g_ft) slot = g_fifo[g_fh++ % 8]; pthread_mutex_unlock(&g_fmu);
    if (slot < 0) return -1;
    if (braw_gpu_wait(g_gpu, slot, K)) return -1;
    g_cur_slot = slot; return 0;
#else
    (void)K; return -1;
#endif
}

/* ---- rate control (FINDINGS 32) --------------------------------------------------------- */
static int g_rmode = 1; static double g_rval = 5, g_scale = 1.0;
void braw_set_rate(int mode, double value) { g_rmode = mode; g_rval = value; if (mode == 0) g_scale = value; }
double braw_last_scale(void) { return g_scale; }
static int g_geo[4 * BRAW_MAX_COLS * 64]; static Buf g_sb[BRAW_MAX_COLS * 64]; static int g_ns;
static void make_geo(const BrawLayout *L) {          /* slice rectangles in file order (column-major) */
    int x0 = 0; g_ns = 0;
    for (int c = 0; c < L->ncols; c++) {
        for (int r = 0; r < L->nrows; r++, g_ns++) { int y0 = r * L->row_h, h = L->coded_h - y0 < L->row_h ? L->coded_h - y0 : L->row_h;
            g_geo[4 * g_ns] = x0; g_geo[4 * g_ns + 1] = y0; g_geo[4 * g_ns + 2] = L->col_w[c]; g_geo[4 * g_ns + 3] = h; }
        x0 += L->col_w[c];
    }
}
static size_t g_fixed[BRAW_MAX_COLS * 64];                    /* per-slice matrix-independent bits (GPU passes) */
typedef struct { const BrawCoefs *K; } FxCtx;
static void fixed_one(void *v, int i) { const BrawCoefs *K = ((FxCtx *)v)->K; g_fixed[i] = braw_slice_fixed_bits(K, g_geo[4 * i], g_geo[4 * i + 1], g_geo[4 * i + 2], g_geo[4 * i + 3]); }
static size_t frame_bits_size(const BrawLayout *L, const BrawCoefs *K, double scale, Buf *tmp) {
    (void)L; (void)tmp; BrawQuant Q; braw_quant_scaled(&Q, scale);
#ifdef BRAW_METAL
    if (g_cur_slot >= 0) {                                     /* count on the GPU (FINDINGS 35) */
        float ys[64], cs[32]; const uint32_t *yb, *cb[2]; braw_recip_steps(&Q, ys, cs);
        if (braw_gpu_acbits(g_gpu, g_cur_slot, ys, cs, &yb, &cb[0], &cb[1]) == 0) {
            size_t total = 0; int bw = K->bw;
            for (int i = 0; i < g_ns; i++) {
                int bx0 = g_geo[4 * i] / 8, by0 = g_geo[4 * i + 1] / 8, nbx = g_geo[4 * i + 2] / 8, nby = g_geo[4 * i + 3] / 8;
                size_t bits = g_fixed[i];
                for (int by = by0; by < by0 + nby; by++) {
                    const uint32_t *r = yb + (size_t)by * bw + bx0; for (int x = 0; x < nbx; x++) bits += r[x];
                    for (int pl = 0; pl < 2; pl++) { const uint32_t *c = cb[pl] + (size_t)by * (bw / 2) + bx0 / 2; for (int x = 0; x < nbx / 2; x++) bits += c[x]; }
                }
                total += (bits + 63) / 64 * 8;
            }
            return total;
        }
    }
#endif
    return braw_count_slices(K, &Q, g_ns, g_geo);              /* count on the CPU */
}
/* smallest matrix scale (finest steps) whose slices fit the budget. Bits fall roughly as scale^-0.75, so the first
   step predicts from that; once bracketed, log-domain secant (clamped inside the bracket). Typically 3-4 passes. */
static int g_passes;
static double choose_scale(const BrawLayout *L, const BrawCoefs *K, size_t budget) {
    double sbad = 0, sgood = 0, s = g_scale > 0 ? g_scale : 1.0; size_t nbad = 0, ngood = 0;
    double target = (double)budget * 0.997;              /* aim just under the budget */
    g_passes = 0;
    for (int it = 0; it < 16; it++) {
        size_t n = frame_bits_size(L, K, s, NULL); g_passes++;
        if (n > budget) { sbad = s; nbad = n; } else { sgood = s; ngood = n; }
        if (sgood > 0 && ngood >= budget * 0.98) break;                             /* within 2 % under budget */
        if (sbad > 0 && sgood > 0) {
            if (sgood / sbad < 1.002) break;
            double a = log((double)nbad), c = log((double)ngood), f = (a - log(target)) / (a - c);
            if (f < 0.05) f = 0.05;
            if (f > 0.95) f = 0.95;
            s = exp(log(sbad) + f * (log(sgood) - log(sbad)));
        } else {
            s = s * pow((double)n / target, 1 / 0.75);
            if (s > 400) s = 400;
            if (s < 0.2) s = 0.2;
            if (sgood > 0 && s >= sgood) break;                                     /* already at the floor */
        }
    }
    if (getenv("DNG2BRAW_PROF")) fprintf(stderr, "rate: %d pass(es)\n", g_passes);
    return sgood > 0 ? sgood : 400;
}

/* ---- the slice coder ------------------------------------------------------------------ */
int braw_encode_slice(BrawCodec codec, const uint16_t *coded, int stride, int x0, int y0, int w, int h,
                      int bits, Buf *out, char *err, size_t errlen) {
    if (codec == BRAW_CODEC_SPLICE) {
        if (!g_sp || g_spk >= g_spn) { snprintf(err, errlen, "splice: reference has %d slices, need more", g_spn); return -1; }
        size_t a = rd32(g_sp + 0x80 + 4 * g_spk), e = sp_end(g_spk); g_spk++;
        bput(out, g_sp + a, e - a); return 0;
    }
    if (codec == BRAW_CODEC_BRAW) {     /* real slices come from braw_encode_slice_planes() via braw_build_frame */
        snprintf(err, errlen, "internal: BRAW slices are encoded from planes (braw_build_frame)"); return -1;
    }
    /* staging slice: 0xFED1 marker, version, geometry, bit depth, then big-endian 16-bit samples */
    bu16(out, 0xFED1); bu16(out, 2);
    bu16(out, x0); bu16(out, y0); bu16(out, w); bu16(out, h); bu16(out, bits); bu16(out, 0);
    for (int y = 0; y < h; y++) {
        const uint16_t *r = coded + (size_t)(y0 + y) * stride + x0;
        size_t at = out->n; bzero_(out, (size_t)w * 2);
        for (int x = 0; x < w; x++) wr16(out->p + at + 2 * x, r[x]);
    }
    return 0;
}

/* ---- per-frame metadata box: 256 bytes, boxes and sizes exactly as the Video Assist writes them */
static void box_u32(Buf *b, const char *t, uint32_t v) { size_t o = astart(b, t); bu32(b, v); aend(b, o); }
static void box_txt(Buf *b, const char *t, const char *s) { size_t o = astart(b, t); char z[24] = {0}; snprintf(z, sizeof z, "%s", s); bput(b, z, 24); aend(b, o); }
static void put_bmdf(Buf *b, const BrawFrameMeta *fm) {
    size_t start = b->n, o = astart(b, "bmdf"), s; char t[32];
    s = astart(b, "srte"); bu32(b, fm->fps_n); bu32(b, fm->fps_d); aend(b, s);
    snprintf(t, sizeof t, "%.1f\xc2\xb0", fm->shutter_angle); box_txt(b, "shtv", fm->shutter_angle > 0 ? t : "");
    box_u32(b, "innd", 0);                                       /* observed 0 */
    s = astart(b, "agpf"); bf32(b, fm->analog_gain > 0 ? fm->analog_gain : 1.0); aend(b, s);
    box_u32(b, "asct", (uint32_t)fm->kelvin);                    /* as-shot kelvin (= wkel in VA files) */
    s = astart(b, "asti"); bu16(b, (uint16_t)(int16_t)fm->tint); aend(b, s);
    if (fm->fnumber > 0) snprintf(t, sizeof t, "F%.1f", fm->fnumber); else t[0] = 0; box_txt(b, "aptr", t);
    box_u32(b, "expo", 0);                                       /* observed 0 */
    if (fm->focal_mm > 0) snprintf(t, sizeof t, "%.0fmm", fm->focal_mm); else t[0] = 0; box_txt(b, "fcln", t);
    box_txt(b, "dsnc", "");                                      /* focus distance: not in the DNG */
    box_u32(b, "isoe", (uint32_t)fm->iso);
    box_u32(b, "wkel", (uint32_t)fm->kelvin);
    s = astart(b, "wtin"); bu16(b, (uint16_t)(int16_t)fm->tint); aend(b, s);
    size_t used = b->n - start;
    if (used + 8 <= BRAW_BMDF_SIZE) { s = astart(b, "skip"); bzero_(b, BRAW_BMDF_SIZE - used - 8); aend(b, s); }
    aend(b, o);
}

/* ---- image payload ------------------------------------------------------------------- */
int braw_build_frame(const BrawLayout *L, BrawCodec codec, const uint16_t *raw, int bits, double black,
                     const BrawFrameMeta *fm, Buf *out, char *err, size_t errlen) {
    /* coded frame: DNG rows placed at pad_top; padding rows copy the nearest DNG rows of the same
       CFA phase (pairs of lines), so the padding carries plausible data */
    int W = L->coded_w, H = L->coded_h;
    static uint16_t *coded; static size_t coded_n;          /* kept across frames */
    static BrawPlanes P; static BrawCoefs Kc; BrawCoefs Kg = {0}, *Kp = &Kc; BrawQuant BQ;   /* kept across frames (FINDINGS 34) */
    int gpu = codec == BRAW_CODEC_BRAW && take_prepared(&Kg) == 0;   /* coefficients already made on the GPU (FINDINGS 35) */
    if (gpu) Kp = &Kg;
    else {
        if (coded_n < (size_t)W * H) { free(coded); coded = malloc((size_t)W * H * 2); coded_n = coded ? (size_t)W * H : 0; }
        if (!coded) { snprintf(err, errlen, "out of memory"); return -1; }
        place_coded(L, raw, coded);
    }
    if (codec == BRAW_CODEC_BRAW) {                            /* FINDINGS 22, 30: planes, then the luma pre-filter */
        if (!gpu && (braw_planes_from_cfa(coded, W, H, black, &P) || braw_prefilter(&P) || braw_coefs(&P, &Kc))) { braw_planes_free(&P); braw_coefs_free(&Kc); snprintf(err, errlen, "out of memory"); return -1; }
        make_geo(L);
        if (g_cur_slot >= 0) { FxCtx fx = { Kp }; braw_par_for(g_ns, fixed_one, &fx); }
        if (g_rmode == 1) {                                    /* budget for the slices: ratio against 12-bit coded size, minus header/bmdf */
            double total = (double)W * H * 12 / 8 / g_rval; size_t budget = total > 8192 ? (size_t)(total - BRAW_HEADER_SIZE - BRAW_BMDF_SIZE - (BRAW_FRAME_ALIGN - 1)) : 4096;
            g_scale = choose_scale(L, Kp, budget);
        }
        braw_quant_scaled(&BQ, g_scale);
        size_t got = braw_encode_slices(Kp, &BQ, g_ns, g_geo, g_sb);       /* final slices, in parallel */
        if (g_rmode == 1) {                                    /* GPU counts can differ from the CPU coder at rounding ties: */
            double total = (double)W * H * 12 / 8 / g_rval;   /* if the real frame is over budget, coarsen slightly */
            size_t budget = total > 8192 ? (size_t)(total - BRAW_HEADER_SIZE - BRAW_BMDF_SIZE - (BRAW_FRAME_ALIGN - 1)) : 4096;
            for (int t = 0; got > budget && t < 20; t++) { g_scale *= 1.002; braw_quant_scaled(&BQ, g_scale); got = braw_encode_slices(Kp, &BQ, g_ns, g_geo, g_sb); }
            if (getenv("DNG2BRAW_PROF")) fprintf(stderr, "final %zu bytes (budget %zu)\n", got, budget);
        }
    }
    size_t f0 = out->n;
    put_bmdf(out, fm);
    size_t p0 = out->n;
    bput(out, codec != BRAW_CODEC_STAGE ? "braw" : "fpbr", 4);
    bu32(out, 0);                                              /* 0x04 payload size, patched below */
    bu8(out, 2); bu8(out, 0);                                  /* 0x08 VA 02; 0x09 quality code (00 = Q0, 08 at 5:1) */
    bu16(out, 0x051f);                                         /* 0x0a observed 0x051f (VA + fp) */
    bu16(out, W); bu16(out, H);                                /* 0x0c coded width, height */
    bu16(out, L->row_h); bu16(out, 0x0004);                    /* 0x10 slice height; 0x12 observed 00 04 */
    bu16(out, 16); bu16(out, 16); bu16(out, 16);               /* 0x14 observed at Q0 (vary at 5:1): quantiser? */
    bu16(out, 270); bu16(out, 266); bu16(out, 280);            /* 0x1a observed at Q0, ISO 800 (0x1c/0x1e follow ISO) */
    bu16(out, 0x0500); bu16(out, 0x0100);                      /* 0x20 observed */
    bzero_(out, 0x40 - 0x24);
    if (codec == BRAW_CODEC_BRAW) {                            /* 5:1 profile (filter table, FINDINGS 32), then this frame's geometry: */
        memcpy(out->p + p0 + 0x08, braw_hdr_5to1, sizeof braw_hdr_5to1);   /* 0x0a strips, 0x0b bands (FINDINGS 40: 5/31 on the VA, */
        out->p[p0 + 0x0a] = (uint8_t)L->ncols; out->p[p0 + 0x0b] = (uint8_t)L->nrows;   /* 3/32 and 4/31 in camera files) */
        wr16(out->p + p0 + 0x0c, (uint16_t)W); wr16(out->p + p0 + 0x0e, (uint16_t)H); wr16(out->p + p0 + 0x10, (uint16_t)L->row_h);
    }
    if (codec == BRAW_CODEC_SPLICE) {                          /* reference's quality and quantiser words */
        memcpy(out->p + p0 + 0x08, g_sp + 0x08, 4); memcpy(out->p + p0 + 0x10, g_sp + 0x10, 0x40 - 0x10); g_spk = 0;
    }
    for (int i = 0; i < BRAW_MAX_COLS; i++) bu16(out, i < L->ncols ? L->col_w[i] : 0);   /* 0x40 strip widths */
    bzero_(out, 0x80 - 0x50);
    int ns = L->ncols * L->nrows;
    size_t table = out->n; bzero_(out, 4 * (size_t)ns);        /* 0x80 slice offsets from payload start */
    bzero_(out, BRAW_HEADER_SIZE - (out->n - p0));
    for (int i = 0; i < 96; i++) wr16(out->p + p0 + 0x1080 + 2 * i, codec == BRAW_CODEC_BRAW ? BQ.q[i] : 33);    /* FINDINGS 20: 0x1080-0x113f, 96 x 33 in every
                                                                          Video Assist + fp frame (Q0 and 5:1, all ISOs): 96 values,
                                                                          e.g. a flat table. Zeros here change the decoded gain. */
    int x0 = 0, k = 0;
    for (int c = 0; c < L->ncols; c++) {                       /* column-major: down each strip */
        for (int r = 0; r < L->nrows; r++, k++) {
            int y0 = r * L->row_h, h = H - y0 < L->row_h ? H - y0 : L->row_h;
            wr32(out->p + table + 4 * k, (uint32_t)(out->n - p0));
            if (codec == BRAW_CODEC_BRAW) bput(out, g_sb[k].p, g_sb[k].n);
            else if (braw_encode_slice(codec, coded, W, x0, y0, L->col_w[c], h, bits, out, err, errlen)) { out->n = f0; return -1; }
        }
        x0 += L->col_w[c];
    }
    
    size_t total = out->n - f0, pad = (BRAW_FRAME_ALIGN - total % BRAW_FRAME_ALIGN) % BRAW_FRAME_ALIGN;
    bzero_(out, pad);
    wr32(out->p + p0 + 4, (uint32_t)(out->n - p0));            /* payload size = sample size - 256 */
    return 0;
}

int braw_stage_decode(const uint8_t *f, size_t len, const BrawLayout *L, uint16_t *raw, char *err, size_t errlen) {
    if (len < BRAW_BMDF_SIZE + BRAW_HEADER_SIZE || memcmp(f + 4, "bmdf", 4)) { snprintf(err, errlen, "no bmdf box"); return -1; }
    const uint8_t *p = f + be32(f);
    size_t plen = len - be32(f);
    if (memcmp(p, "fpbr", 4)) { snprintf(err, errlen, "not a staging frame"); return -1; }
    if (be32(p + 4) != plen) { snprintf(err, errlen, "payload size %u != %zu", be32(p + 4), plen); return -1; }
    if ((int)be16(p + 12) != L->coded_w || (int)be16(p + 14) != L->coded_h) { snprintf(err, errlen, "size mismatch"); return -1; }
    int ns = L->ncols * L->nrows;
    for (int k = 0; k < ns; k++) {
        uint32_t o = be32(p + 0x80 + 4 * k);
        if (o + 16 > plen) { snprintf(err, errlen, "slice %d out of range", k); return -1; }
        const uint8_t *s = p + o;
        if (be16(s) != 0xFED1) { snprintf(err, errlen, "slice %d: no marker", k); return -1; }
        int x0 = be16(s + 4), y0 = be16(s + 6), w = be16(s + 8), h = be16(s + 10);
        if (x0 + w > L->coded_w || y0 + h > L->coded_h || o + 16 + (size_t)w * h * 2 > plen) { snprintf(err, errlen, "slice %d: bad geometry", k); return -1; }
        s += 16;
        for (int y = 0; y < h; y++, s += 2 * (size_t)w) {
            int dy = y0 + y - L->pad_top;
            if (dy < 0 || dy >= L->h) continue;                /* padding lines */
            for (int x = 0; x < w; x++) raw[(size_t)dy * L->w + x0 + x] = be16(s + 2 * x);
        }
    }
    return 0;
}

/* 122-byte sample entry: visual sample entry (camera files carry uninitialised bytes in the
   reserved fields; zeros here) + bver=1, ctrn=1, bfdn (rate denominator), in the Video Assist's order */
int braw_parse_desqueeze(const char *s, uint32_t *h, uint32_t *v, double *f) {
    double x = atof(s); if (!(x >= 1.0 && x <= 4.0)) return 0;
    static const struct { double f; uint32_t h, v; } k[] = { {1.25, 5, 4}, {4.0 / 3, 4, 3}, {1.5, 3, 2}, {1.6, 8, 5}, {5.0 / 3, 5, 3}, {1.8, 9, 5}, {2.0, 2, 1} };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (fabs(x - k[i].f) < 0.011) { *h = k[i].h; *v = k[i].v; *f = k[i].f; return 1; }
    *h = (uint32_t)lround(x * 100); *v = 100; *f = x; return 1;
}
static uint32_t g_pasp_h = 1, g_pasp_v = 1;      /* pixel aspect for anamorphic clips (--desqueeze) */
void braw_set_pixel_aspect(uint32_t h, uint32_t v) { g_pasp_h = h ? h : 1; g_pasp_v = v ? v : 1; }
static uint32_t g_bfdn = 1001;                   /* frame-rate denominator: 1001 for NTSC rates, 1 for whole rates (FINDINGS 42) */
void braw_set_rate_denominator(uint32_t d) { g_bfdn = d ? d : 1; }
/* the SDK wants a whole rate as N/1 and an NTSC rate as N*1000/1001: 48000/1000 opens but every frame after 0 fails */
void braw_normalise_rate(int *n, int *d) {
    if (*n <= 0 || *d <= 0) return;
    double r = (double)*n / *d, k = nearbyint(r * 1.001);
    if (fabs(r - nearbyint(r)) < 1e-4) { *n = (int)nearbyint(r); *d = 1; return; }
    if (k > 0 && fabs(r - k / 1.001) < 1e-3) { *n = (int)k * 1000; *d = 1001; return; }
    int a = *n, b = *d; while (b) { int t = a % b; a = b; b = t; } *n /= a; *d /= a;
}
void braw_put_sample_entry(Buf *b, BrawCodec codec, int w, int h) {
    size_t en = astart(b, codec != BRAW_CODEC_STAGE ? "brvl" : "fpbr");
    bzero_(b, 6); bu16(b, 1);
    bu16(b, 0); bu16(b, 0); bzero_(b, 12);
    bu16(b, w); bu16(b, h); put_fixed16(b, 72); put_fixed16(b, 72); bu32(b, 0); bu16(b, 1);
    bzero_(b, 32);
    bu16(b, 0); bu16(b, 0xFFFF);
    size_t o;
    o = astart(b, "bver"); bu32(b, 1); aend(b, o);
    o = astart(b, "ctrn"); bu32(b, 1); aend(b, o);
    o = astart(b, "bfdn"); bu32(b, g_bfdn); aend(b, o);
    if (g_pasp_h != g_pasp_v) { o = astart(b, "pasp"); bu32(b, g_pasp_h); bu32(b, g_pasp_v); aend(b, o); }
    aend(b, en);
}
