/*
 * dng_view.c: CinemaDNG playback helper for cDNG Raw Convert's viewer.
 *
 *   dng_view [--look display|film|film709|film2020] LIST.txt   (LIST.txt: the clip's DNG files, one path per line, in order)
 *
 * Prints one JSON line about the clip ({"width":..,"height":..,"frames":..}), then reads commands on stdin,
 * one per line, exactly like braw_view:
 *   f INDEX SCALE    develop frame INDEX at 1/SCALE resolution (1, 2, 4 or 8); several may be in flight
 *   q                finish and quit
 * Each frame goes to stdout as "BRVF", u32 index, u32 width, u32 height (little endian), then RGBA8; a frame
 * that can't be read goes out as "BRVE", u32 index, u32 error, u32 0.
 *
 * This is a fast monitoring development, not a grade: the DNG's own black/white levels, linearisation, as-shot
 * white balance and colour matrices (the fpSup DNG reader shared with dng2braw), the default crop, a 2x2
 * "superpixel" debayer at half resolution and below (bilinear at full), BaselineExposure, then one of the looks
 * below (a display rendering, or Blackmagic Film Gen 5 log). Frames are read and developed on several threads at once, so playback isn't
 * limited by one core (Core Image's RAW filter manages about 25-35 fps on these files; this does well over 100).
 *
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include "dng_frontend.h"

static char **g_files; static int g_nf;
static pthread_mutex_t g_out = PTHREAD_MUTEX_INITIALIZER;

/* ---- request queue */
typedef struct { uint32_t index, scale; } Req;
static Req g_q[4096]; static int g_qh, g_qt, g_quit;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_qc = PTHREAD_COND_INITIALIZER;

/* ---- colour, from the first frame
 * Looks (--look):
 *   display  Rec.709/sRGB for the screen: extended-Reinhard roll-off that reaches white exactly at the sensor's clip
 *            (nothing is cut off below it), a gentle contrast curve and a little extra saturation;
 *   film     Blackmagic Film Generation 5 (log) in Blackmagic Wide Gamut Gen 4/5, flat, for log LUTs;
 *   film709, film2020  the same log curve with Rec.709 or Rec.2020 primaries: the app turns it back into
 *            scene-linear for LUTs that expect linear input (8-bit log carries the whole range; 8-bit linear wouldn't).
 * Highlights: each white-balanced channel stops at the level where the first channel clips, so blown areas go
 * to neutral white instead of magenta.
 * Scene-linear scale: the DNG's own BaselineExposure (+3 EV on the fp) puts middle grey near 0.18. */
enum { LOOK_DISPLAY, LOOK_FILM, LOOK_FILM709, LOOK_FILM2020 };
static int g_look = LOOK_DISPLAY;
static pthread_mutex_t g_cm = PTHREAD_MUTEX_INITIALIZER; static int g_have_colour;
static float g_K[9];                  /* white-balanced camera (black-subtracted) -> scene-linear RGB of the look */
static float g_wb[3], g_cap;          /* white balance multipliers; highlight cap (camera units) */
static float g_xmax;                  /* top of the LUT's input range (scene-linear) */
#define NLUT 32768
static uint8_t g_lut[NLUT];           /* indexed by sqrt(x / xmax): fine steps in the shadows */

static double gen5_oetf(double x) {   /* Blackmagic Design, "Blackmagic Generation 5 Color Science" (2021) */
    const double A = 0.08692876065491224, B = 0.005494072432257808, C = 0.5300133392291939, D = 8.283605932402494, E = 0.09246575342465753;
    return x < 0.005 ? D * x + E : A * log(x + B) + C;
}
static void mat3_mul(const double *a, const double *b, double *o) { for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) { double s = 0; for (int j = 0; j < 3; j++) s += a[3*r+j] * b[3*j+k]; o[3*r+k] = s; } }
static void mat3_inv(const double *m, double *o) {
    double d = m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
    o[0] = (m[4]*m[8]-m[5]*m[7])/d; o[1] = (m[2]*m[7]-m[1]*m[8])/d; o[2] = (m[1]*m[5]-m[2]*m[4])/d;
    o[3] = (m[5]*m[6]-m[3]*m[8])/d; o[4] = (m[0]*m[8]-m[2]*m[6])/d; o[5] = (m[2]*m[3]-m[0]*m[5])/d;
    o[6] = (m[3]*m[7]-m[4]*m[6])/d; o[7] = (m[1]*m[6]-m[0]*m[7])/d; o[8] = (m[0]*m[4]-m[1]*m[3])/d;
}

static void setup_colour(const Frame *f) {
    pthread_mutex_lock(&g_cm);
    if (!g_have_colour) {
        Colour c = dngf_colour_params(f);
        static const double XYZ2SRGB[9] = { 3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252 };
        /* Blackmagic Wide Gamut (Gen 4/5): R 0.7177215,0.3171181  G 0.2280410,0.8615690  B 0.1005841,-0.0820452, D65 */
        static const double BMDWG2XYZ[9] = { 0.6065303722131459, 0.2204080953375982, 0.12347900045836878,
                                             0.26798940707297414, 0.8327308786223493, -0.1007202856953232,
                                             -0.029442166015309742, -0.08661060696630066, 1.2048607644426022 };
        double XYZ2BMD[9]; mat3_inv(BMDWG2XYZ, XYZ2BMD);
        double range = f->white - f->black; if (range <= 0) range = 4095;
        /* film looks: 0.35 stop under the DNG's own scale, which is how Blackmagic's decoder exposes a .braw made by
           dng2braw from the same clip (measured), so a clip looks the same as DNG and as BRAW */
        double ev = getenv("DNG_VIEW_EV") ? atof(getenv("DNG_VIEW_EV")) : (g_look == LOOK_DISPLAY ? 0 : -0.35);
        double gain = pow(2.0, f->baseline_exposure + ev) / range;
        g_wb[0] = (float)c.wb_r; g_wb[1] = 1; g_wb[2] = (float)c.wb_b;
        g_cap = (float)(range * fmin(1.0, fmin(c.wb_r, c.wb_b)));      /* the first channel to clip, white-balanced */
        double T[9];
        static const double XYZ2REC2020[9] = { 1.7166511880, -0.3556707838, -0.2533662814, -0.6666843518, 1.6164812366, 0.0157685458, 0.0176398574, -0.0427706133, 0.9421031212 };
        if (g_look == LOOK_FILM) mat3_mul(XYZ2BMD, c.M, T);
        else if (g_look == LOOK_FILM2020) mat3_mul(XYZ2REC2020, c.M, T);
        else mat3_mul(XYZ2SRGB, c.M, T);
        if (g_look == LOOK_DISPLAY) {          /* a little extra saturation, as camera-style renderings have */
            double sat = getenv("DNG_VIEW_SAT") ? atof(getenv("DNG_VIEW_SAT")) : 1.35, S[9], L[3] = { 0.2126, 0.7152, 0.0722 }, U[9];
            for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) S[3*r+k] = (1 - sat) * L[k] + (r == k ? sat : 0);
            mat3_mul(S, T, U); memcpy(T, U, sizeof T);
        }
        for (int i = 0; i < 9; i++) g_K[i] = (float)(T[i] * gain);
        double white = gain * g_cap;                                   /* scene-linear level of a clipped white */
        g_xmax = (float)(white * 1.6);
        double con = getenv("DNG_VIEW_CON") ? atof(getenv("DNG_VIEW_CON")) : 0.55;
        for (int i = 0; i < NLUT; i++) {
            double u = (double)i / (NLUT - 1), x = u * u * g_xmax, y;
            if (g_look == LOOK_DISPLAY) {
                double a = 1.5, Lw = a * white, xa = a * x;             /* a: brightens the mid-tones */
                y = xa * (1 + xa / (Lw * Lw)) / (1 + xa);               /* extended Reinhard: the sensor's clip -> 1 */
                y = fmin(1, y);
                y = y <= 0.0031308 ? 12.92 * y : 1.055 * pow(y, 1 / 2.4) - 0.055;
                y = (1 - con) * y + con * y * y * (3 - 2 * y);          /* gentle S-curve for contrast */
            } else y = gen5_oetf(x);
            g_lut[i] = (uint8_t)lrint(fmin(1, fmax(0, y)) * 255);
        }
        g_have_colour = 1;
    }
    pthread_mutex_unlock(&g_cm);
}
static inline uint8_t enc(float v) {
    if (v <= 0) return g_lut[0];
    float u = sqrtf(v / g_xmax); int i = (int)(u * (NLUT - 1) + 0.5f);
    return g_lut[i > NLUT - 1 ? NLUT - 1 : i];
}
/* r, g, b: black-subtracted camera values */
static inline void put(uint8_t *o, float r, float g, float b) {
    r = fminf(r * g_wb[0], g_cap); g = fminf(g, g_cap); b = fminf(b * g_wb[2], g_cap);
    o[0] = enc(g_K[0]*r + g_K[1]*g + g_K[2]*b); o[1] = enc(g_K[3]*r + g_K[4]*g + g_K[5]*b);
    o[2] = enc(g_K[6]*r + g_K[7]*g + g_K[8]*b); o[3] = 255;
}

/* develop the crop (x0,y0,cw,ch; all even, RGGB at x0,y0) at 1/scale */
static uint8_t *develop(const Frame *f, int x0, int y0, int cw, int ch, int scale, int *ow, int *oh) {
    const uint16_t *raw = f->raw; const int W = f->w; const float bl = (float)f->black;
    if (scale >= 2) {
        int q = scale / 2;                                  /* quads per output pixel, each way */
        int w = cw / scale, h = ch / scale; if (w < 1 || h < 1) return NULL;
        uint8_t *px = malloc((size_t)w * h * 4); float inv = 1.0f / (q * q);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < q; j++) {
                const uint16_t *p0 = raw + (size_t)(y0 + 2 * (y * q + j)) * W + x0 + 2 * x * q, *p1 = p0 + W;
                for (int i = 0; i < q; i++, p0 += 2, p1 += 2) { r += p0[0]; g += p0[1] + p1[0]; b += p1[1]; }
            }
            put(px + 4 * ((size_t)y * w + x), r * inv - bl, g * inv * 0.5f - bl, b * inv - bl);
        }
        *ow = w; *oh = h; return px;
    }
    int w = cw, h = ch; uint8_t *px = malloc((size_t)w * h * 4);
    #define V(xx, yy) ((float)raw[(size_t)(y0 + ((yy) < 0 ? 1 : (yy) >= h ? h - 2 : (yy))) * W + x0 + ((xx) < 0 ? 1 : (xx) >= w ? w - 2 : (xx))])
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float v = V(x, y), r, g, b;
        int ry = !(y & 1), rx = !(x & 1);
        if (ry && rx)       { r = v; g = (V(x-1,y) + V(x+1,y) + V(x,y-1) + V(x,y+1)) * 0.25f; b = (V(x-1,y-1) + V(x+1,y-1) + V(x-1,y+1) + V(x+1,y+1)) * 0.25f; }
        else if (ry)        { g = v; r = (V(x-1,y) + V(x+1,y)) * 0.5f; b = (V(x,y-1) + V(x,y+1)) * 0.5f; }
        else if (rx)        { g = v; r = (V(x,y-1) + V(x,y+1)) * 0.5f; b = (V(x-1,y) + V(x+1,y)) * 0.5f; }
        else                { b = v; g = (V(x-1,y) + V(x+1,y) + V(x,y-1) + V(x,y+1)) * 0.25f; r = (V(x-1,y-1) + V(x+1,y-1) + V(x-1,y+1) + V(x+1,y+1)) * 0.25f; }
        put(px + 4 * ((size_t)y * w + x), r - bl, g - bl, b - bl);
    }
    #undef V
    *ow = w; *oh = h; return px;
}

static void emit_error(uint32_t index, uint32_t code) {
    uint32_t hd[3] = { index, code, 0 };
    pthread_mutex_lock(&g_out); fwrite("BRVE", 1, 4, stdout); fwrite(hd, 4, 3, stdout); fflush(stdout); pthread_mutex_unlock(&g_out);
}
static void crop_of(const Frame *f, int *x0, int *y0, int *cw, int *ch) {
    int l = f->crop[0], r = f->crop[1], t = f->crop[2], b = f->crop[3];
    if (l < 0 || r < 0 || t < 0 || b < 0 || l + r >= f->w || t + b >= f->h) l = r = t = b = 0;
    *x0 = l & ~1; *y0 = t & ~1; *cw = (f->w - r - *x0) & ~1; *ch = (f->h - b - *y0) & ~1;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_qm);
        while (g_qh == g_qt && !g_quit) pthread_cond_wait(&g_qc, &g_qm);
        if (g_qh == g_qt && g_quit) { pthread_mutex_unlock(&g_qm); return NULL; }
        Req r = g_q[g_qh]; g_qh = (g_qh + 1) % 4096;
        pthread_mutex_unlock(&g_qm);
        Frame f;
        if ((int)r.index >= g_nf) { emit_error(r.index, 1); continue; }
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {    /* a busy RAID or USB drive can fail a read: try again */
            if (attempt) usleep(20000 * attempt);
            if (!dngf_load_frame(g_files[r.index], &f, 1)) { if (f.raw) ok = 1; else dngf_free_frame(&f); }
        }
        if (!ok) { emit_error(r.index, 1); continue; }
        if (f.bits <= 0 || f.w < 4 || f.h < 4) { dngf_free_frame(&f); emit_error(r.index, 2); continue; }
        setup_colour(&f);
        int x0, y0, cw, ch, w = 0, h = 0; crop_of(&f, &x0, &y0, &cw, &ch);
        int s = r.scale >= 8 ? 8 : r.scale >= 4 ? 4 : r.scale >= 2 ? 2 : 1;
        uint8_t *px = develop(&f, x0, y0, cw, ch, s, &w, &h);
        dngf_free_frame(&f);
        if (!px) { emit_error(r.index, 3); continue; }
        uint32_t hd[3] = { r.index, (uint32_t)w, (uint32_t)h };
        pthread_mutex_lock(&g_out);
        fwrite("BRVF", 1, 4, stdout); fwrite(hd, 4, 3, stdout); fwrite(px, 1, (size_t)w * h * 4, stdout); fflush(stdout);
        pthread_mutex_unlock(&g_out);
        free(px);
    }
}

int main(int argc, char **argv) {
    const char *listpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--look") && i + 1 < argc) { const char *l = argv[++i]; g_look = !strcmp(l, "film") ? LOOK_FILM : !strcmp(l, "film709") ? LOOK_FILM709 : !strcmp(l, "film2020") ? LOOK_FILM2020 : LOOK_DISPLAY; }
        else listpath = argv[i];
    }
    if (!listpath) { fprintf(stderr, "usage: dng_view [--look display|film|film709|film2020] LIST.txt\n"); return 2; }
    FILE *lf = fopen(listpath, "r"); if (!lf) { printf("{\"error\":\"can't read the frame list\"}\n"); return 1; }
    char line[4096]; int cap = 0;
    while (fgets(line, sizeof line, lf)) {
        size_t n = strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (g_nf == cap) { cap = cap ? cap * 2 : 1024; g_files = realloc(g_files, sizeof *g_files * cap); }
        g_files[g_nf++] = strdup(line);
    }
    fclose(lf);
    if (!g_nf) { printf("{\"error\":\"no DNG frames\"}\n"); return 1; }
    Frame f0; if (dngf_load_frame(g_files[0], &f0, 0)) { printf("{\"error\":\"the first frame isn't a DNG this viewer can read\"}\n"); fflush(stdout); return 1; }
    int x0, y0, cw, ch; crop_of(&f0, &x0, &y0, &cw, &ch);
    printf("{\"width\":%d,\"height\":%d,\"frames\":%d}\n", cw, ch, g_nf); fflush(stdout);
    dngf_free_frame(&f0);
    long nc = sysconf(_SC_NPROCESSORS_ONLN); int nt = nc > 2 ? (int)nc - 1 : 2; if (nt > 12) nt = 12;
    if (getenv("DNG_VIEW_THREADS")) nt = atoi(getenv("DNG_VIEW_THREADS")) > 0 ? atoi(getenv("DNG_VIEW_THREADS")) : nt;
    pthread_t th[16]; for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, NULL);
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'q') break;
        unsigned idx = 0, scale = 2;
        if (line[0] == 'f' && sscanf(line + 1, "%u %u", &idx, &scale) >= 1) {
            pthread_mutex_lock(&g_qm);
            int nx = (g_qt + 1) % 4096;
            if (nx != g_qh) { g_q[g_qt].index = idx; g_q[g_qt].scale = scale; g_qt = nx; pthread_cond_signal(&g_qc); }
            pthread_mutex_unlock(&g_qm);
            if (nx == g_qh) emit_error(idx, 4);
        } else if (line[0] == 'c') {                      /* cancel: drop everything still queued */
            pthread_mutex_lock(&g_qm); g_qh = g_qt; pthread_mutex_unlock(&g_qm);
        }
    }
    pthread_mutex_lock(&g_qm); g_quit = 1; g_qh = g_qt; pthread_cond_broadcast(&g_qc); pthread_mutex_unlock(&g_qm);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    return 0;
}
