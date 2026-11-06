/*
 * dng2braw_cli.c: SIGMA fp CinemaDNG clip -> Blackmagic RAW layout (proof of concept).
 *
 * Part of dng2braw (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Experimental; NOT Blackmagic software, and not endorsed by Blackmagic Design.
 *
 * What works: the DNG front end (shared with dng2prr), generic colour from the DNG's own
 * calibration (which matches Blackmagic's fp colour within 0.8 dE2000, FINDINGS 9), and the
 * BRAW container and frame structure exactly as a Video Assist 12G writes them for the fp
 * (FINDINGS 7): 122-byte 'brvl' entry, edit lists, clip metadata keys, timecode track,
 * 256-byte 'bmdf' box, frame header, 5 strips x 72-line slices column-major, 256-byte padding.
 * What doesn't: the BRAW slice bitstream. So real .braw output stops with an explanation,
 * and --stage writes the same layout with uncompressed slices (.fpbraw) for testing.
 *
 * Build:  cc -O2 -o dng2braw dng2braw_cli.c braw_format.c dng_frontend.c dng_jpeg.c dng_lj92.c -lm
 * Usage:  dng2braw OUT.fpbraw CLIP_FOLDER --stage [--verify] [--limit N] [--fps N/D] [--neutral R/G,B/G]
 *         dng2braw OUT.braw CLIP_FOLDER            (reports that the BRAW encoder isn't available yet)
 *         dng2braw --dry-run CLIP_FOLDER            (what would be written: layout, colour, metadata)
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include "dng_frontend.h"
#include "braw_format.h"
#include "braw_encode.h"
#include "braw_wb.h"
#include "fsg2.h"

#define VERSION "0.15"

static void die(const char *fmt, const char *a) { fprintf(stderr, "dng2braw: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n"); exit(1); }
static double now(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec * 1e-6; }
static int ends_with_ci(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static long tc_frames(const uint8_t tc[8], int nominal, int *drop) {            /* same as dng2prr */
    int ff = (tc[0] & 0x0F) + 10 * ((tc[0] >> 4) & 0x03);
    int ss = (tc[1] & 0x0F) + 10 * ((tc[1] >> 4) & 0x07);
    int mm = (tc[2] & 0x0F) + 10 * ((tc[2] >> 4) & 0x07);
    int hh = (tc[3] & 0x0F) + 10 * ((tc[3] >> 4) & 0x03);
    *drop = (tc[0] >> 6) & 1;
    long total = ((long)hh * 3600 + mm * 60 + ss) * nominal + ff;
    if (*drop && (nominal == 30 || nominal == 60)) { int dpm = nominal / 15; long mins = hh * 60L + mm; total -= dpm * (mins - mins / 10); }
    return total;
}

static char **list_dngs(char **inputs, int ninputs, int *nout) {
    char **files = NULL; int nf = 0;
    for (int i = 0; i < ninputs; i++) {
        if (is_dir(inputs[i])) {
            DIR *dir = opendir(inputs[i]); struct dirent *e; char **l = NULL; int n = 0;
            while (dir && (e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".dng")) {
                size_t L = strlen(inputs[i]) + strlen(e->d_name) + 2; l = realloc(l, sizeof(char *) * (n + 1)); l[n] = malloc(L); snprintf(l[n++], L, "%s/%s", inputs[i], e->d_name); }
            if (dir) closedir(dir);
            if (n) qsort(l, n, sizeof(char *), cmpstr);
            files = realloc(files, sizeof(char *) * (nf + n + 1)); if (n) memcpy(files + nf, l, sizeof(char *) * n); nf += n; free(l);
        } else { files = realloc(files, sizeof(char *) * (nf + 1)); files[nf++] = inputs[i]; }
    }
    *nout = nf; return files;
}

/* generic colour: white balance as a correlated colour temperature from the DNG's own
   calibration (dng2prr's maths); tint 0. The Blackmagic sensor profile for the fp, and so
   real Blackmagic colour, comes from the Video Assist test clips (docs/TEST-PLAN). */
/* Video Assist + fp: analog_gain = ISO/800 x 2^(1/3) exactly (1.259921 at 800, 5.039684 at 3200) */
static void frame_meta(const Frame *f, const Colour *c, int fps_n, int fps_d, BrawFrameMeta *fm) {
    memset(fm, 0, sizeof *fm);
    fm->fps_n = fps_n; fm->fps_d = fps_d;
    if (f->exposure_time > 0) { fm->shutter_angle = 360.0 * f->exposure_time * fps_n / fps_d; if (fm->shutter_angle > 360) fm->shutter_angle = 360; }
    fm->iso = f->iso; fm->analog_gain = f->iso > 0 ? f->iso / 800.0 * cbrt(2.0) : 1.0;   /* Video Assist: ISO 800 -> 2^(1/3), 3200 -> 4 x 2^(1/3) */
    fm->kelvin = c->cct; fm->tint = 0; fm->fnumber = f->fnumber; fm->focal_mm = f->focal;
    int wk, wt;                       /* Blackmagic-style as-shot WB from the AsShotNeutral (FINDINGS 37); generic CCT otherwise */
    if (braw_wb_from_neutral(f->neutral, &wk, &wt) == 0) { fm->kelvin = wk; fm->tint = wt; }
}

/* read-ahead: the next DNG is loaded on a thread while the current frame encodes */
typedef struct { const char *path; Frame f; int rc; const BrawLayout *L; int prepare; } Prefetch;
static void *prefetch_run(void *a) {      /* load the next DNG, then start its GPU stages (if any) */
    Prefetch *p = a; p->rc = dngf_load_frame(p->path, &p->f, 1);
    if (!p->rc && p->prepare) braw_prepare_frame(p->L, p->f.raw, p->f.black);
    return NULL;
}
/* write-behind: a frame is written on a thread while the next one is built */
typedef struct { FILE *fo; const Buf *b; int ok; } Writer;
static void *writer_run(void *a) { Writer *w = a; w->ok = fwrite(w->b->p, 1, w->b->n, w->fo) == w->b->n; return NULL; }
static int g_rate_mode = 1, g_qlabel = -1; static double g_rate_val = 5;   /* default: 5:1 */
static const char *rate_label(void) {
    static char t[32];
    if (g_rate_mode == 1) snprintf(t, sizeof t, "%g:1", g_rate_val);
    else if (g_qlabel >= 0) snprintf(t, sizeof t, "Q%d", g_qlabel);
    else snprintf(t, sizeof t, "dng2braw qscale %g", g_rate_val);
    return t;
}
static double g_line_time_us = 0;
static int g_viewing_film = 1;      /* --viewing film (default): open as Blackmagic Film Gen 5 / Blackmagic Wide Gamut; rec709: as the Video Assist */
static double g_desqueeze = 1.0;    /* --desqueeze: anamorphic factor, written as Blackmagic's anamorphic keys + 'pasp' */   /* sensor_line_time: the fp's readout over the picture's rows (FINDINGS 41) */
static void clip_meta(Meta *m, const Frame *f, const Colour *c, const BrawLayout *L, BrawCodec codec, const char *clipname, double bitrate) {
    /* keys and order as a Video Assist 12G writes them for the fp (FINDINGS 7). camera_type is the
       name Blackmagic's software uses for the fp; the encoder keys say who really wrote the file. */
    meta_str(m, "camera_type", "SIGMA fp L");
    meta_str(m, "manufacturer", "SIGMA");
    meta_u16(m, "viewing_bmdgen", g_viewing_film ? 5 : 4);
    if (g_desqueeze > 1.0) {        /* keys and types as a Blackmagic camera writes them (u16 flag, text factor) */
        char t[16]; if (fabs(g_desqueeze - nearbyint(g_desqueeze)) < 1e-6) snprintf(t, sizeof t, "%.1fx", g_desqueeze); else snprintf(t, sizeof t, "%.4gx", g_desqueeze);
        meta_u16(m, "anamorphic_enable", 1); meta_str(m, "anamorphic", t);
    }
    meta_size(m, "crop_origin", f->crop[0], f->crop[2] + L->pad_top);
    meta_size(m, "crop_size", L->w - f->crop[0] - f->crop[1], L->h - f->crop[2] - f->crop[3]);
    meta_size(m, "safe_area", 0, 0);
    /* With gyro, the sensor area is the picture: telemetry-parser at d45ebf2 (the Gyroflow (fpSup) plugin v0.1.10)
       takes the readout as sensor_area_captured's rows x sensor_line_time, and later versions (19bc11f, Gyroflow 1.6.3)
       as the picture's rows x it; only equal rows give both the fp's real readout (cDNG Raw Convert FINDINGS 15). Without gyro, as the
       Video Assist reports the fp. */
    if (g_line_time_us > 0) meta_size(m, "sensor_area_captured", L->w - f->crop[0] - f->crop[1], L->h - f->crop[2] - f->crop[3]);
    else if (L->w == 3856 && L->h == 2170) meta_size(m, "sensor_area_captured", 6000, 3376);   /* fp UHD, as the Video Assist reports it */
    else meta_size(m, "sensor_area_captured", L->coded_w, L->coded_h);
    meta_str(m, "braw_compression_ratio", codec == BRAW_CODEC_BRAW ? rate_label() : codec == BRAW_CODEC_SPLICE ? "dng2braw splice test" : "uncompressed (dng2braw staging)");
    meta_u32(m, "braw_codec_bitrate", (uint32_t)(bitrate > 4e9 ? 4e9 : bitrate));
    meta_str(m, "encoder_device_manufacturer", "fpSup community");
    meta_str(m, "encoder_device_name", "dng2braw");
    meta_str(m, "encoder_device_version", VERSION);
    meta_str(m, "viewing_gamma", g_viewing_film ? "Blackmagic Design Film" : "Rec.709");   /* as Blackmagic cameras shooting Film */
    meta_str(m, "viewing_gamut", g_viewing_film ? "Blackmagic Design" : "Rec.709");
    meta_f32(m, "sensor_line_time", g_line_time_us);
    meta_str(m, "reel_name", clipname);
    meta_f32(m, "analog_gain", f->iso > 0 ? f->iso / 800.0 * cbrt(2.0) : 1.0);
    meta_u16(m, "analog_gain_is_constant", 1);
    /* the DNG's own colour, kept so later versions (and checks) can map it */
    const char *P = "io.github.fpsup.dng2braw.";
    char k[96];
    #define K(s) (snprintf(k, sizeof k, "%s%s", P, s), k)
    meta_str(m, K("format"), "Blackmagic RAW (a Blackmagic Design format), written by dng2braw, not by Blackmagic Design software");
    { int wk, wt; meta_str(m, K("colour"), braw_wb_from_neutral(f->neutral, &wk, &wt) == 0
        ? "as-shot white balance from the DNG AsShotNeutral, as Blackmagic processing for the fp reads it (measured)"
        : "generic: white balance from the DNG calibration as CCT, tint 0"); }
    if (f->lens[0]) meta_str(m, K("lens"), f->lens);
    meta_u16(m, K("dng_bits"), f->bits);
    meta_f32(m, K("dng_black_level"), f->black);
    meta_f32(m, K("dng_white_level"), f->white);
    meta_f32v(m, K("dng_as_shot_neutral"), f->neutral, 3);
    meta_f32v(m, K("dng_color_matrix_1"), f->cm1, 9);
    meta_f32v(m, K("dng_color_matrix_2"), f->cm2, 9);
    meta_u16(m, K("dng_illuminant_1"), f->illum1);
    meta_u16(m, K("dng_illuminant_2"), f->illum2);
    meta_f32(m, K("dng_baseline_exposure"), f->baseline_exposure);
    meta_f32v(m, K("camera_to_xyz_d65"), c->M, 9);
    meta_u32(m, K("as_shot_cct"), (uint32_t)c->cct);
    meta_u16(m, K("coded_pad_top"), L->pad_top);
    #undef K
}

/* ---- audio: the clip folder's WAV, written as the Video Assist writes it (FINDINGS 39):
   24-bit little-endian 'in24' PCM in one-second chunks, interleaved with the frames */
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
typedef struct { FILE *fh; int ch, sw, rate; uint32_t frames; long data_off; } Wav;
static int wav_open(const char *path, Wav *w) {
    memset(w, 0, sizeof *w); w->fh = fopen(path, "rb"); if (!w->fh) return -1;
    uint8_t h[12]; if (fread(h, 1, 12, w->fh) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(w->fh); return -1; }
    for (;;) {
        uint8_t c[8]; if (fread(c, 1, 8, w->fh) != 8) { fclose(w->fh); return -1; }
        uint32_t n = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) { uint8_t fm[40] = {0}; if (fread(fm, 1, n < 40 ? n : 40, w->fh) < 16) { fclose(w->fh); return -1; }
            if (n > 40) fseek(w->fh, n - 40, SEEK_CUR);
            int tag = le16(fm); if (tag == 0xFFFE && n >= 26) tag = le16(fm + 24);       /* extensible: the sub-format */
            if (tag != 1) { fclose(w->fh); return -2; }                                   /* integer PCM only */
            w->ch = le16(fm + 2); w->rate = le32(fm + 4); w->sw = le16(fm + 14) / 8; if (n & 1) fseek(w->fh, 1, SEEK_CUR); }
        else if (!memcmp(c, "data", 4)) { w->data_off = ftell(w->fh); w->frames = w->ch && w->sw ? n / (w->ch * w->sw) : 0;
            return w->ch > 0 && w->ch <= 16 && w->sw >= 1 && w->sw <= 4 && w->rate > 0 ? 0 : -2; }
        else fseek(w->fh, n + (n & 1), SEEK_CUR);
    }
}
/* append n sample frames from the WAV as in24 (24-bit little endian); silence past its end */
static int wav_put(Wav *w, uint32_t first, uint32_t n, Buf *b) {
    size_t in = (size_t)n * w->ch * w->sw, outn = (size_t)n * w->ch * 3; uint8_t *t = malloc(in ? in : 1);
    size_t got = 0;
    if (first < w->frames) { fseek(w->fh, w->data_off + (long)first * w->ch * w->sw, SEEK_SET); got = fread(t, 1, in, w->fh); }
    memset(t + got, w->sw == 1 ? 0x80 : 0, in - got);
    size_t at = b->n; bzero_(b, outn); uint8_t *o = b->p + at;
    for (size_t k = 0; k < (size_t)n * w->ch; k++, o += 3) {
        const uint8_t *q = t + k * w->sw;
        if (w->sw == 1) { o[0] = 0; o[1] = 0; o[2] = (uint8_t)(q[0] - 128); }
        else if (w->sw == 2) { o[0] = 0; o[1] = q[0]; o[2] = q[1]; }
        else if (w->sw == 3) { o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; }
        else { o[0] = q[1]; o[1] = q[2]; o[2] = q[3]; }
    }
    free(t); return 0;
}
static void pad_to(Buf *b, uint64_t pos, uint64_t align) { uint64_t e = pos + b->n; if (e % align) bzero_(b, (size_t)(align - e % align)); }

static void put_edts(Buf *b, uint32_t dur) {             /* one edit: the whole track, as in Video Assist files */
    size_t e = astart(b, "edts"), l = astart(b, "elst"); bu32(b, 0); bu32(b, 1); bu32(b, dur); bu32(b, 0); bu32(b, 0x10000); aend(b, l); aend(b, e);
}

static void usage(void) {
    fprintf(stderr,
        "dng2braw %s (fpSup proof of concept; not Blackmagic software)\n"
        "usage: dng2braw OUT.fpbraw CLIP_FOLDER|FILES.dng... --stage [--verify] [--limit N] [--fps N/D]\n"
        "                [--neutral R/G,B/G] [--progress-every N]\n"
        "         [--wav FILE | --no-audio]   audio: the clip folder's WAV by default (24-bit PCM, as the Video Assist)\n"
        "         [--no-gyro] [--gyro-jsonl]   fpSup Gyro2 data goes into a Gyroflow Protobuf track (as dng2prr)\n"
        "         [--viewing film|rec709]   how Resolve and Blackmagic software open it: Blackmagic Film Gen 5 /\n"
        "                                    Blackmagic Wide Gamut (flat, the default) or Rec.709 (as the Video Assist)\n"
        "         [--desqueeze F]   anamorphic factor (1.25, 1.33, 1.5, 1.6, 1.66, 1.8, 2): pixel aspect + Blackmagic anamorphic keys\n"
        "         BRAW container and frame layout with uncompressed slices, for testing\n"
        "       dng2braw OUT.braw CLIP_FOLDER [--ratio N | --quality Q0..Q5 | --qscale S] [--limit N]\n"
        "         real Blackmagic RAW (fp UHD). --ratio N: each frame fits N:1 of the 12-bit size (default 5);\n"
        "         --cpu: don't use the GPU (macOS Metal builds use it for the transform stages)\n"
        "         tuning (defaults are the measured best): --rounding R (0.33) --edge-filter 0-3 (2) --chroma-factor F (1)\n"
        "         --quality: constant quality, matrix scale 1 / 1.4 / 2 / 2.8 / 4 / 5.6 (Q0 = the Video Assist's matrix)\n"
        "       dng2braw OUT.braw CLIP_FOLDER --splice REF.payload   developer test: real BRAW with the DNG's\n"
        "         metadata and slices copied from a Video Assist frame (tools/braw_payload.py); picture = reference\n"
        "       dng2braw --dry-run CLIP_FOLDER   print the layout, colour and metadata it would write\n"
        "       dng2braw --list-clips FOLDER   print every clip folder (folder of DNG frames) at or below FOLDER\n"
        "       dng2braw --version\n", VERSION);
    exit(2);
}

/* --list-clips PATH: every folder at or below PATH that holds DNG frames, one absolute path per line (for the app) */
static int has_dng(const char *dirpath) {
    DIR *d = opendir(dirpath); struct dirent *e; int r = 0;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".dng")) { r = 1; break; }
    if (d) closedir(d);
    return r;
}
static void list_clips(const char *path, int depth) {
    if (depth > 12) return;
    if (has_dng(path)) { char rp[4096]; printf("%s\n", realpath(path, rp) ? rp : path); return; }   /* a clip: don't descend */
    DIR *d = opendir(path); struct dirent *e; char **names = NULL; int n = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[4096]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name); struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) { names = realloc(names, sizeof *names * (n + 1)); names[n++] = strdup(e->d_name); }
    }
    if (d) closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (int i = 0; i < n; i++) { char sub[4096]; snprintf(sub, sizeof sub, "%s/%s", path, names[i]); list_clips(sub, depth + 1); free(names[i]); }
    free(names);
}

/* --probe CLIP_FOLDER: one JSON line describing the clip, from its first frame only (for the app's media pool) */
static void json_str(const char *k, const char *v, int comma) {
    printf("%s\"%s\":\"", comma ? "," : "", k);
    for (const char *c = v; *c; c++) { if (*c == '"' || *c == '\\') putchar('\\'); if ((unsigned char)*c >= 32) putchar(*c); }
    putchar('"');
}
static int probe_clip(const char *dir) {
    char *in[1] = { (char *)dir }; int nf = 0; char **files = list_dngs(in, 1, &nf);
    if (!nf) { printf("{\"error\":\"no DNG frames\"}\n"); return 1; }
    Frame f; if (dngf_load_frame(files[0], &f, 0)) { printf("{\"error\":\"can't read the first DNG\"}\n"); return 1; }
    int fps_n = f.fps_n > 0 ? f.fps_n : 24, fps_d = f.fps_d > 0 ? f.fps_d : 1;
    braw_normalise_rate(&fps_n, &fps_d);
    Colour col = dngf_colour_params(&f); BrawFrameMeta fm; frame_meta(&f, &col, fps_n, fps_d, &fm);
    int pw = f.w - f.crop[0] - f.crop[1], ph = f.h - f.crop[2] - f.crop[3]; if (pw <= 0 || ph <= 0) { pw = f.w; ph = f.h; }
    BrawLayout L; int braw_ok = !braw_layout(f.w, f.h, &L) && braw_size_tested(L.coded_w, L.coded_h);
    Fsg2Frame g; memset(&g, 0, sizeof g); char e[256] = ""; int gyro = fsg2_read(files[0], &g, e, sizeof e) == 0 && g.present;
    char wav[1024] = ""; { DIR *d = opendir(dir); struct dirent *de; char *best = NULL;
        while (d && (de = readdir(d))) if (de->d_name[0] != '.' && ends_with_ci(de->d_name, ".wav")) if (!best || strcmp(de->d_name, best) < 0) { free(best); best = strdup(de->d_name); }
        if (d) closedir(d);
        if (best) { snprintf(wav, sizeof wav, "%s", best); free(best); } }
    char tc[64] = "";
    int nom = (int)nearbyint((double)fps_n / fps_d);
    if (f.has_tc && nom > 0) { int drop = 0; long t = tc_frames(f.tc, nom, &drop);
        snprintf(tc, sizeof tc, "%02ld:%02ld:%02ld%c%02ld", t / (3600L * nom), t / (60L * nom) % 60, t / nom % 60, drop ? ';' : ':', t % nom); }
    const char *mode = pw == 3840 && ph == 2160 ? "UHD" : pw == 4096 && ph == 2160 ? "DCI 4K" : pw == 1920 && ph == 1080 ? "FHD"
                     : pw == 3008 && ph == 2000 ? "OG3K" : (pw < 2200 && ph * 3 > pw * 2 - 64 && ph * 3 < pw * 2 + 64) ? "OG2K" : "";
    printf("{\"frames\":%d,\"width\":%d,\"height\":%d,\"picture_width\":%d,\"picture_height\":%d,\"bits\":%d,", nf, f.w, f.h, pw, ph, f.bits);
    printf("\"fps_num\":%d,\"fps_den\":%d,\"iso\":%d,\"kelvin\":%d,\"tint\":%d,\"gyro\":%s,\"braw_supported\":%s", fps_n, fps_d, fm.iso, fm.kelvin, fm.tint, gyro ? "true" : "false", braw_ok ? "true" : "false");
    json_str("mode", mode, 1); json_str("timecode", tc, 1); json_str("wav", wav, 1); json_str("first_frame", files[0], 1);
    json_str("lens", f.lens, 1);
    printf(",\"shutter_angle\":%.1f,\"fnumber\":%.2f,\"focal_mm\":%.1f,\"exposure_s\":%.6f,\"black\":%.2f,\"white\":%.0f,\"neutral\":[%.4f,%.4f,%.4f],\"baseline_exposure\":%.3f",
           fm.shutter_angle, fm.fnumber, fm.focal_mm, f.exposure_time, f.black, f.white, f.neutral[0], f.neutral[1], f.neutral[2], f.baseline_exposure);
    printf(",\"crop\":[%d,%d,%d,%d]", f.crop[0], f.crop[1], f.crop[2], f.crop[3]);
    if (gyro) printf(",\"imu_hz\":%.1f,\"readout_ms\":%.3f", g.period_ps ? 1e12 / g.period_ps : 0, g.readout_ns / 1e6);
    if (braw_ok) printf(",\"braw_coded\":[%d,%d]", L.coded_w, L.coded_h);
    printf("}\n");
    fsg2_free(&g);
    dngf_free_frame(&f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--list-clips")) { list_clips(argv[2], 0); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--probe")) return probe_clip(argv[2]);
    const char *out = NULL; char **inputs = malloc(sizeof(char *) * argc); int ninputs = 0;
    int stage = 0, verify = 0, dry = 0, limit = 0, every = 25, fps_n = 0, fps_d = 0, have_neutral = 0;
    double neutral[2] = { 1, 1 }; const char *splice = NULL, *wavp = NULL; int no_audio = 0, no_gyro = 0, gyro_jsonl = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (usage(), ""))
        if (!strcmp(a, "--stage")) stage = 1;
        else if (!strcmp(a, "--verify")) verify = 1;
        else if (!strcmp(a, "--splice")) splice = NEXT();
        else if (!strcmp(a, "--dry-run")) dry = 1;
        else if (!strcmp(a, "--ratio")) { g_rate_mode = 1; g_rate_val = atof(NEXT()); if (g_rate_val < 1.5 || g_rate_val > 40) usage(); }
        else if (!strcmp(a, "--quality")) { const char *q = NEXT(); int qi = (q[0] == 'Q' || q[0] == 'q') ? atoi(q + 1) : atoi(q); if (qi < 0 || qi > 5) usage();
                                            static const double qs[6] = {1.0, 1.4, 2.0, 2.8, 4.0, 5.6}; g_rate_mode = 0; g_rate_val = qs[qi]; g_qlabel = qi; }
        else if (!strcmp(a, "--cpu")) braw_set_gpu(0);
        else if (!strcmp(a, "--rounding")) braw_set_rounding(atof(NEXT()));
        else if (!strcmp(a, "--edge-filter")) braw_set_edge_strength(atoi(NEXT()));
        else if (!strcmp(a, "--raw-gains")) { if (sscanf(NEXT(), "%lf,%lf,%lf", &braw_raw_gain[0], &braw_raw_gain[1], &braw_raw_gain[2]) != 3) usage(); }
        else if (!strcmp(a, "--chroma-factor")) braw_set_chroma_factor(atof(NEXT()));
        else if (!strcmp(a, "--qscale")) { g_rate_mode = 0; g_rate_val = atof(NEXT()); g_qlabel = -1; if (g_rate_val <= 0) usage(); }
        else if (!strcmp(a, "--limit")) limit = atoi(NEXT());
        else if (!strcmp(a, "--progress-every")) { every = atoi(NEXT()); if (every < 1) every = 1; }
        else if (!strcmp(a, "--fps")) { if (sscanf(NEXT(), "%d/%d", &fps_n, &fps_d) != 2 || fps_n <= 0 || fps_d <= 0) usage(); }
        else if (!strcmp(a, "--neutral")) { if (sscanf(NEXT(), "%lf,%lf", &neutral[0], &neutral[1]) != 2) usage(); have_neutral = 1; }
        else if (!strcmp(a, "--wav")) wavp = NEXT();
        else if (!strcmp(a, "--no-audio")) no_audio = 1;
        else if (!strcmp(a, "--no-gyro")) no_gyro = 1;
        else if (!strcmp(a, "--viewing")) { const char *v = NEXT(); if (!strcmp(v, "film")) g_viewing_film = 1; else if (!strcmp(v, "rec709")) g_viewing_film = 0; else usage(); }
        else if (!strcmp(a, "--desqueeze")) { uint32_t ph, pv; if (!braw_parse_desqueeze(NEXT(), &ph, &pv, &g_desqueeze)) usage(); braw_set_pixel_aspect(ph, pv); }
        else if (!strcmp(a, "--gyro-jsonl")) gyro_jsonl = 1;
        else if (!strcmp(a, "--version")) { printf("dng2braw %s (fpSup)\n", VERSION); return 0; }
        else if (a[0] == '-' && a[1] == '-') usage();
        else if (!out && !dry) out = a;
        else inputs[ninputs++] = (char *)a;
    }
    if (!ninputs || (!out && !dry)) usage();
    BrawCodec codec = stage ? BRAW_CODEC_STAGE : splice ? BRAW_CODEC_SPLICE : BRAW_CODEC_BRAW;
    if (splice) {
        FILE *sf = fopen(splice, "rb"); if (!sf) die("can't read %s", splice);
        fseeko(sf, 0, SEEK_END); long n = ftello(sf); fseeko(sf, 0, SEEK_SET);
        uint8_t *sp = malloc(n); if (fread(sp, 1, n, sf) != (size_t)n) die("%s: short read", splice); fclose(sf);
        char e[256]; if (braw_set_splice(sp, n, e, sizeof e)) die("%s", e);
    }
    if (out && stage && ends_with_ci(out, ".braw")) die("%s: staging output isn't Blackmagic RAW; name it .fpbraw", out);
    if (out && !stage && !ends_with_ci(out, ".braw")) die("%s: real output must be .braw (or add --stage for .fpbraw)", out);

    int nf; char **files = list_dngs(inputs, ninputs, &nf);
    if (limit > 0 && nf > limit) nf = limit;
    if (!nf) die("%s", "no DNG files");
    Frame first; if (dngf_load_frame(files[0], &first, 0)) return 1;
    if (have_neutral) { first.neutral[0] = neutral[0]; first.neutral[1] = 1; first.neutral[2] = neutral[1]; }
    if (!fps_n) { fps_n = first.fps_n; fps_d = first.fps_d; }
    if (fps_n <= 0 || fps_d <= 0) { fps_n = 24; fps_d = 1; }
    braw_normalise_rate(&fps_n, &fps_d); braw_set_rate_denominator((uint32_t)fps_d);
    BrawLayout L; if (braw_layout(first.w, first.h, &L)) { fprintf(stderr, "dng2braw: %dx%d: this recording mode isn't supported by the BRAW encoder yet (no slice layout for this size)\n", first.w, first.h); return 3; }
    Colour col = dngf_colour_params(&first);
    BrawFrameMeta fm0; frame_meta(&first, &col, fps_n, fps_d, &fm0);
    char clipname[256]; { const char *s = ninputs ? inputs[0] : files[0]; char t[4096]; snprintf(t, sizeof t, "%s", s);
        size_t n = strlen(t); while (n > 1 && t[n-1] == '/') t[--n] = 0; const char *b = strrchr(t, '/'); snprintf(clipname, sizeof clipname, "%s", b ? b + 1 : t);
        char *dot = strrchr(clipname, '.'); if (dot && !is_dir(s)) *dot = 0; }

    printf("%d frame(s), %dx%d, %d-bit, black %g, white %g, fps %d/%d\n", nf, first.w, first.h, first.bits, first.black, first.white, fps_n, fps_d);
    printf("coded %dx%d (%d line(s) above the DNG); slices: %d strip(s) (", L.coded_w, L.coded_h, L.pad_top, L.ncols);
    for (int i = 0; i < L.ncols; i++) printf("%s%d", i ? ", " : "", L.col_w[i]);
    printf(") x %d bands of %d lines, column-major\n", L.nrows, L.row_h);
    printf("white balance: %d K, tint %+d, ISO %d, shutter %.1f deg, F%.1f, %.0fmm\n", fm0.kelvin, fm0.tint, fm0.iso, fm0.shutter_angle, fm0.fnumber, fm0.focal_mm);
    /* audio: --wav FILE, else the first .wav (by name) in the clip folder, as the fp records it */
    Wav wav; int have_wav = 0; char wavbuf[4096];
    if (!no_audio && !wavp && ninputs && is_dir(inputs[0])) {
        DIR *dir = opendir(inputs[0]); struct dirent *e; char *best = NULL;
        while (dir && (e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".wav")) if (!best || strcmp(e->d_name, best) < 0) { free(best); best = strdup(e->d_name); }
        if (dir) closedir(dir);
        if (best) { snprintf(wavbuf, sizeof wavbuf, "%s/%s", inputs[0], best); wavp = wavbuf; free(best); }
    }
    uint32_t a_total = 0;
    if (!no_audio && wavp) {
        if (wav_open(wavp, &wav) == 0) {
            have_wav = 1;
            double vid = (double)nf * fps_d * wav.rate / fps_n;            /* the picture's length in samples */
            a_total = wav.frames < vid ? wav.frames : (uint32_t)nearbyint(vid);
            printf("audio: %s (%d ch, %d-bit, %d Hz, %.2f s; %.2f s written)\n", wavp, wav.ch, wav.sw * 8, wav.rate, (double)wav.frames / wav.rate, (double)a_total / wav.rate);
            if (!a_total) have_wav = 0;
        } else fprintf(stderr, "dng2braw: warning: could not read %s as integer PCM WAV; writing without audio\n", wavp);
    }
    /* fpSup Gyro2: each DNG's FSG2 block -> one Gyroflow Protobuf message per frame (as dng2prr, FINDINGS 41).
       Each frame's message is written just before the frame, so the first (with the "GyroflowProtobuf" header that
       telemetry-parser looks for in the first and last 5 MB) sits right after the timecode sample. */
    uint8_t **gmsg = NULL; size_t *glen = NULL; int have_gyro = 0;
    uint32_t m_rate = 0, m_n = 0; float (*m_g)[3] = NULL, (*m_a)[3] = NULL;   /* Blackmagic motion readings */
    if (!no_gyro && codec == BRAW_CODEC_BRAW) {
        Fsg2Frame *gf = calloc(nf, sizeof(Fsg2Frame)); int ngot = 0, bad = 0; char gerr[640] = "";
        for (int i = 0; i < nf; i++) {
            char e1[256] = "";
            if (fsg2_read(files[i], &gf[i], e1, sizeof e1) < 0) { if (!bad) snprintf(gerr, sizeof gerr, "%s: %s", files[i], e1); bad++; }
            else if (!gf[i].present && e1[0] && !gerr[0]) snprintf(gerr, sizeof gerr, "%s", e1);
            ngot += gf[i].present;
            if (!i && !gf[0].present && !bad) break;          /* no Gyro2 in this clip */
        }
        if (ngot && ngot < nf) fprintf(stderr, "dng2braw: warning: only %d of %d frames carry Gyro2 data%s%s; writing without gyro telemetry\n", ngot, nf, gerr[0] ? ": " : "", gerr);
        else if (ngot == nf) {
            FILE *js = NULL; char jsp[4096] = "";
            if (gyro_jsonl && out) { snprintf(jsp, sizeof jsp, "%s", out); char *dot = strrchr(jsp, '.'), *sl = strrchr(jsp, '/'); if (dot && (!sl || dot > sl)) *dot = 0; strncat(jsp, ".jsonl", sizeof jsp - strlen(jsp) - 1); js = fopen(jsp, "w"); }
            int pw = first.w - first.crop[0] - first.crop[1], ph = first.h - first.crop[2] - first.crop[3];   /* the decoded picture: DefaultCrop */
            if (pw <= 0 || ph <= 0) { pw = first.w; ph = first.h; }
            if (fsg2_messages(gf, nf, pw, ph, &gmsg, &glen, js, gerr, sizeof gerr) == 0) {
                long ns = 0; for (int i = 0; i < nf; i++) ns += gf[i].n_samples;
                have_gyro = 1;
                printf("gyro: fpSup Gyro2 data in every frame (%ld IMU samples at %.0f Hz), kept as a Gyroflow Protobuf track\n", ns, 1e12 / gf[0].period_ps);
                if (js) printf("gyro: also written to %s\n", jsp);
            } else {
                fprintf(stderr, "dng2braw: warning: Gyro2 data could not be converted (%s); writing without gyro telemetry\n", gerr);
                if (js) { fclose(js); js = NULL; unlink(jsp); }
            }
            if (js) fclose(js);
        } else if (bad) fprintf(stderr, "dng2braw: warning: %s; writing without gyro telemetry\n", gerr);
        if (have_gyro) {               /* the same readings as Blackmagic motion tracks (FINDINGS 41) */
            Fsg2Imu im; char e2[256];
            if (fsg2_imu(gf, nf, &im, e2, sizeof e2) == 0 && im.n > 1) {
                double R = nearbyint((im.n - 1) * 1e6 / (im.ts_us[im.n - 1] - im.ts_us[0]));
                double D = nf > 1 ? (im.frame_start_us[nf - 1] - im.frame_start_us[0]) / (nf - 1) : 1e6 / im.fps;
                double vfr = (double)fps_n / fps_d, t0 = im.frame_start_us[0] - im.exposure_us / 2;   /* frame 0's first row, mid-exposure */
                m_rate = (uint32_t)R; m_n = (uint32_t)floor((double)nf / vfr * R);
                m_g = malloc(sizeof(float[3]) * m_n); m_a = malloc(sizeof(float[3]) * m_n);
                long i0 = 0;
                for (uint32_t j = 0; j < m_n; j++) {
                    double ts = t0 + (j / R) * vfr * D;        /* track time j/R -> camera clock */
                    while (i0 + 1 < im.n - 1 && im.ts_us[i0 + 1] <= ts) i0++;
                    double u = (ts - im.ts_us[i0]) / (im.ts_us[i0 + 1] - im.ts_us[i0]); if (u < 0) u = 0; if (u > 1) u = 1;
                    float g[3], a[3];
                    for (int c = 0; c < 3; c++) { g[c] = (float)(im.gyro[i0][c] + u * (im.gyro[i0 + 1][c] - im.gyro[i0][c])); a[c] = im.accel[u < 1 ? i0 : i0 + 1][c]; }
                    const float d2r = (float)(M_PI / 180.0);    /* Blackmagic axes: Gyroflow reads them as "yxz" (FINDINGS 41) */
                    m_g[j][0] = g[1] * d2r; m_g[j][1] = g[0] * d2r; m_g[j][2] = g[2] * d2r;
                    m_a[j][0] = -a[1]; m_a[j][1] = -a[0]; m_a[j][2] = -a[2];
                }
                int rows = (first.h - first.crop[2] - first.crop[3]) / 8 * 8; if (rows <= 0) rows = first.h / 8 * 8;
                g_line_time_us = im.readout_us / rows;
                printf("gyro: also as Blackmagic motion tracks (%u readings at %u Hz; readout %.3f ms)\n", m_n, m_rate, im.readout_us / 1000);
            } else fprintf(stderr, "dng2braw: warning: no Blackmagic motion tracks (%s)\n", e2);
            fsg2_imu_free(&im);
        }
        for (int i = 0; i < nf; i++) fsg2_free(&gf[i]);
        free(gf);
    }
    if (dry) {
        Meta m = { .n = 0 }; clip_meta(&m, &first, &col, &L, codec, clipname, 0);
        printf("clip metadata keys (%d):\n", m.n); for (int i = 0; i < m.n; i++) printf("  %s\n", m.it[i].key);
        return 0;
    }
    braw_set_rate(g_rate_mode, g_rate_val);
    if (codec == BRAW_CODEC_BRAW && !braw_size_tested(L.coded_w, L.coded_h) && !getenv("DNG2BRAW_ANY_SIZE")) {   /* fail before creating anything */
        fprintf(stderr, "dng2braw: %dx%d: this recording mode isn't supported by the BRAW encoder yet\n", first.w, first.h); return 3;
    }

    int tc_drop = 0, tc_nom = (int)nearbyint((double)fps_n / fps_d); long tc_start = 0; int have_tc = first.has_tc && tc_nom > 0;
    if (have_tc) tc_start = tc_frames(first.tc, tc_nom, &tc_drop);

    char partial[4096]; snprintf(partial, sizeof partial, "%s.partial", out);
    FILE *fo = fopen(partial, "wb"); if (!fo) die("can't write %s", partial);
    /* wide + mdat header (made 64-bit at the end if needed), data from 4096 as in camera files */
    Buf b = {0}; bu32(&b, 8); bstr(&b, "wide"); bu32(&b, 0); bstr(&b, "mdat"); bzero_(&b, 4096 - 16);
    uint64_t tc_off = 0;
    if (have_tc) { tc_off = b.n; bu32(&b, (uint32_t)tc_start); }      /* camera files: the timecode sample sits at 4096 */
    fwrite(b.p, 1, b.n, fo); uint64_t pos = b.n; b.n = 0;

    uint64_t *voff = malloc(8 * (size_t)nf); uint32_t *vsz = malloc(4 * (size_t)nf);
    uint64_t *goff = have_gyro ? malloc(8 * (size_t)nf) : NULL;
    enum { MCH = 1024 }; uint32_t m_done = 0; int m_c = 0, m_nch = m_n ? (int)((m_n + MCH - 1) / MCH) : 0;
    uint64_t *moff[2] = { malloc(8 * (size_t)(m_nch + 1)), malloc(8 * (size_t)(m_nch + 1)) }; uint32_t *mcn = malloc(4 * (size_t)(m_nch + 1));
    int a_nch = have_wav ? (int)((a_total + wav.rate - 1) / wav.rate) : 0, a_n = 0; uint32_t a_done = 0;
    uint64_t *aoff = malloc(8 * (size_t)(a_nch + 1)); uint32_t *asz = malloc(4 * (size_t)(a_nch + 1));
    Buf frb[2] = {{0}, {0}}; double t0 = now(); char err[512];
    Writer wr = { fo, NULL, 1 }; pthread_t wth; int have_wth = 0;
    Prefetch pf = { files[0] }; pthread_t pth; int have_pth = 0;
    pf.L = &L; pf.prepare = codec == BRAW_CODEC_BRAW;
    if (codec == BRAW_CODEC_BRAW) printf("GPU: %s\n", braw_gpu_status(&L));
    prefetch_run(&pf);
    double w_load = 0, w_build = 0, w_write = 0; int prof = getenv("DNG2BRAW_PROF") != NULL;
    for (int i = 0; i < nf; i++) {
        double ta = now();
        if (have_pth) { pthread_join(pth, NULL); have_pth = 0; }
        w_load += now() - ta;
        Frame f = pf.f; int rc = pf.rc;
        if (rc) { fclose(fo); unlink(partial); die("%s: can't read", files[i]); }
        if (i + 1 < nf) { pf.path = files[i + 1]; have_pth = !pthread_create(&pth, NULL, prefetch_run, &pf); if (!have_pth) prefetch_run(&pf); }
        if (f.w != first.w || f.h != first.h) { fclose(fo); unlink(partial); die("%s: different size from the first frame - not one clip", files[i]); }
        if (have_neutral) { f.neutral[0] = neutral[0]; f.neutral[1] = 1; f.neutral[2] = neutral[1]; }
        Colour c = dngf_colour_params(&f); BrawFrameMeta fm; frame_meta(&f, &c, fps_n, fps_d, &fm);
        Buf *frp = &frb[i & 1]; frp->n = 0; double tb = now();
        if (have_gyro) { goff[i] = pos; bput(frp, gmsg[i], glen[i]); }       /* this frame's gyro message, then the frame */
        size_t vstart = frp->n;
        if (braw_build_frame(&L, codec, f.raw, f.bits, f.black, &fm, frp, err, sizeof err)) { fclose(fo); unlink(partial); die("%s", err); }
        w_build += now() - tb;
        dngf_free_frame(&f);
        voff[i] = pos + vstart; vsz[i] = (uint32_t)(frp->n - vstart);
        if (have_wav) {                    /* each whole second of sound once the picture has passed it */
            uint32_t need = i == nf - 1 ? a_total : (uint32_t)fmin((double)a_total, floor((double)(i + 1) * fps_d * wav.rate / fps_n));
            while (a_done < a_total && (a_done + (uint32_t)wav.rate <= need || i == nf - 1)) {
                uint32_t n = a_total - a_done < (uint32_t)wav.rate ? a_total - a_done : (uint32_t)wav.rate;
                pad_to(frp, pos, 4096); aoff[a_n] = pos + frp->n; asz[a_n++] = n;
                wav_put(&wav, a_done, n, frp); a_done += n; pad_to(frp, pos, 4096);
            }
        }
        if (m_n) {                         /* motion readings in chunks of 1024, as the camera writes them */
            double tend = (double)(i + 1) * fps_d / fps_n;
            while (m_done < m_n && (m_done + MCH <= (uint32_t)(tend * m_rate) || i == nf - 1)) {
                uint32_t n = m_n - m_done < MCH ? m_n - m_done : MCH;
                for (int t = 0; t < 2; t++) {
                    moff[t][m_c] = pos + frp->n;
                    for (uint32_t j = m_done; j < m_done + n; j++) {
                        const float *v = t ? m_a[j] : m_g[j]; uint8_t smp[20]; wr32(smp, 20); memcpy(smp + 4, t ? "moac" : "mogy", 4);
                        for (int c = 0; c < 3; c++) { uint32_t u; memcpy(&u, &v[c], 4); smp[8 + 4 * c] = u; smp[9 + 4 * c] = u >> 8; smp[10 + 4 * c] = u >> 16; smp[11 + 4 * c] = u >> 24; }
                        bput(frp, smp, 20);
                    }
                }
                mcn[m_c++] = n; m_done += n;
            }
        }
        double tw = now();
        if (have_wth) { pthread_join(wth, NULL); have_wth = 0; w_write += now() - tw; if (!wr.ok) { fclose(fo); unlink(partial); die("%s", "write failed (disk full?)"); } }
        wr.b = frp; have_wth = !pthread_create(&wth, NULL, writer_run, &wr);
        if (!have_wth) writer_run(&wr);
        pos += frp->n;
        Buf fr = *frp;
        if (i % every == 0 || i == nf - 1) { double el = now() - t0; if (codec == BRAW_CODEC_BRAW) printf("  frame %d/%d  %.2f MB  matrix scale %.3f  (%.1f fps)\n", i + 1, nf, fr.n / 1e6, braw_last_scale(), (i + 1) / (el > 0 ? el : 1e-9)); else printf("  frame %d/%d  %.2f MB  (%.1f fps)\n", i + 1, nf, fr.n / 1e6, (i + 1) / (el > 0 ? el : 1e-9)); fflush(stdout); }
    }
    if (have_wth) { pthread_join(wth, NULL); have_wth = 0; }
    if (prof) fprintf(stderr, "main thread per frame: waiting for DNG %.1f ms, building %.1f ms, waiting for write %.1f ms\n", 1e3 * w_load / nf, 1e3 * w_build / nf, 1e3 * w_write / nf);
    if (!wr.ok) { fclose(fo); unlink(partial); die("%s", "write failed (disk full?)"); }
    uint64_t mdat_end = pos, mdsz = mdat_end - 8;
    if (mdsz <= 0xFFFFFFFFu) { uint8_t s4[4]; wr32(s4, (uint32_t)mdsz); fseeko(fo, 8, SEEK_SET); fwrite(s4, 1, 4, fo); }
    else { uint8_t h[16]; wr32(h, 1); memcpy(h + 4, "mdat", 4); wr32(h + 8, (uint32_t)(mdat_end >> 32)); wr32(h + 12, (uint32_t)mdat_end);
           fseeko(fo, 0, SEEK_SET); fwrite(h, 1, 16, fo); }
    fseeko(fo, (off_t)mdat_end, SEEK_SET);

    /* moov: video, timecode, then clip metadata (the order of camera files) */
    uint32_t mv_dur = (uint32_t)nf * fps_d; size_t o;
    size_t moov = astart(&b, "moov");
    o = astart(&b, "mvhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, fps_n); bu32(&b, mv_dur); put_fixed16(&b, 1.0);
    bu16(&b, 0x100); bzero_(&b, 10); put_matrix(&b); bzero_(&b, 24); bu32(&b, 2 + have_tc + have_wav + have_gyro + (m_c ? 2 : 0)); aend(&b, o);
    {
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0xF); bu32(&b, 0); bu32(&b, 0); bu32(&b, 1); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, L.w - first.crop[0] - first.crop[1]); put_fixed16(&b, L.h - first.crop[2] - first.crop[3]); aend(&b, o);
        put_edts(&b, mv_dur);
        if (have_tc) { size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "tmcd"); bu32(&b, 2); aend(&b, t2); aend(&b, tf); }
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, fps_n); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "vide", "Apple Video Media Handler");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "vmhd"); bu32(&b, 1); bu16(&b, 0x40); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0x8000); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Apple Alias Data Handler"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1); braw_put_sample_entry(&b, codec, L.w - first.crop[0] - first.crop[1], L.h - first.crop[2] - first.crop[3]); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, nf); bu32(&b, fps_d); aend(&b, o);
        put_stsc_ones(&b);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 0); bu32(&b, nf); for (int i = 0; i < nf; i++) bu32(&b, vsz[i]); aend(&b, o);
        put_co64(&b, voff, nf);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_tc) {
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0x3); bu32(&b, 0); bu32(&b, 0); bu32(&b, 2); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 20); aend(&b, o);
        put_edts(&b, mv_dur);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, fps_n); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "tmcd", "Time Code Media Handler");
        size_t mi = astart(&b, "minf");
        size_t gm = astart(&b, "gmhd");
        o = astart(&b, "gmin"); bu32(&b, 0); bu16(&b, 0x40); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        size_t tm = astart(&b, "tmcd");
        o = astart(&b, "tcmi"); bu32(&b, 0); bu16(&b, 0); bu16(&b, 0); bu16(&b, 12); bu16(&b, 0);
        bu16(&b, 0xFFFF); bu16(&b, 0xFFFF); bu16(&b, 0xFFFF); bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bu8(&b, 13); bstr(&b, "Lucida Grande"); aend(&b, o);
        aend(&b, tm); aend(&b, gm);
        put_hdlr(&b, "dhlr", "alis", "Apple Alias Data Handler"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "tmcd"); bzero_(&b, 6); bu16(&b, 1); bu32(&b, 0);
        bu32(&b, (tc_drop ? 0x1 : 0) | 0x2); bu32(&b, fps_n); bu32(&b, fps_d); bu8(&b, tc_nom); bu8(&b, 0);
        aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, 1); bu32(&b, mv_dur); aend(&b, o);
        put_stsc_ones(&b);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 4); bu32(&b, 1); aend(&b, o);
        put_co64(&b, &tc_off, 1);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_wav) {                        /* sound track, laid out as in Video Assist files */
        uint32_t tid = have_tc ? 3 : 2, a_mdur = (uint32_t)nearbyint((double)a_total * fps_n / wav.rate);
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0xF); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid); bu32(&b, 0); bu32(&b, a_mdur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0x100); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        put_edts(&b, a_mdur);
        if (have_tc) { size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "tmcd"); bu32(&b, 2); aend(&b, t2); aend(&b, tf); }
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, wav.rate); bu32(&b, a_total); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "soun", "Apple Sound Media Handler");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "smhd"); bu32(&b, 0); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Apple Alias Data Handler"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "in24"); bzero_(&b, 6); bu16(&b, 1);
        bu16(&b, 1); bu16(&b, 0); bu32(&b, 0);                              /* sound description version 1 */
        bu16(&b, wav.ch); bu16(&b, 16); bu16(&b, 0xFFFF); bu16(&b, 0);
        bu32(&b, (uint32_t)wav.rate << 16);
        bu32(&b, 1); bu32(&b, 3); bu32(&b, 3 * wav.ch); bu32(&b, 2);
        { size_t wv = astart(&b, "wave"); size_t q = astart(&b, "frma"); bstr(&b, "in24"); aend(&b, q);
          q = astart(&b, "enda"); bu16(&b, 1); aend(&b, q); bu32(&b, 8); bu32(&b, 0); aend(&b, wv); }
        aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, a_total); bu32(&b, 1); aend(&b, o);
        { int ne = 0; for (int k = 0; k < a_n; k++) if (!k || asz[k] != asz[k - 1]) ne++;
          o = astart(&b, "stsc"); bu32(&b, 0); bu32(&b, ne);
          for (int k = 0; k < a_n; k++) if (!k || asz[k] != asz[k - 1]) { bu32(&b, k + 1); bu32(&b, asz[k]); bu32(&b, 1); }
          aend(&b, o); }
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 1); bu32(&b, a_total); aend(&b, o);
        put_co64(&b, aoff, a_n);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    for (int t = 0; t < 2 && m_c; t++) {   /* Blackmagic motion tracks, laid out as a Blackmagic camera writes them (FINDINGS 41) */
        static const char *key[2] = { "com.blackmagicdesign.motiondata.gyroscope", "com.blackmagicdesign.motiondata.accelerometer" };
        uint32_t tid = 2 + have_tc + have_wav + t, m_mdur = (uint32_t)nearbyint((double)m_n * fps_n / m_rate);
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0xF); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid); bu32(&b, 0); bu32(&b, m_mdur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        put_edts(&b, m_mdur);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, m_rate); bu32(&b, m_n); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "meta", "Timed Metadata Media Handler");
        size_t mi = astart(&b, "minf");
        { size_t gm = astart(&b, "gmhd"); o = astart(&b, "gmin"); bu32(&b, 0); bu16(&b, 0x40); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0); bu16(&b, 0); aend(&b, o); aend(&b, gm); }
        put_hdlr(&b, "dhlr", "alis", "Apple Alias Data Handler"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        { size_t en = astart(&b, "mebx"); bzero_(&b, 6); bu16(&b, 1);
          size_t ks = astart(&b, "keys"); size_t ke = astart(&b, t ? "moac" : "mogy");
          size_t q = astart(&b, "keyd"); bstr(&b, "fiel"); bstr(&b, key[t]); aend(&b, q);
          q = astart(&b, "dtyp"); bu32(&b, 1); bstr(&b, key[t]); aend(&b, q);
          q = astart(&b, "flps"); bu32(&b, 3); aend(&b, q);
          aend(&b, ke); aend(&b, ks); aend(&b, en); }
        aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, m_n); bu32(&b, 1); aend(&b, o);
        { int ne = 0; for (int k = 0; k < m_c; k++) if (!k || mcn[k] != mcn[k - 1]) ne++;
          o = astart(&b, "stsc"); bu32(&b, 0); bu32(&b, ne);
          for (int k = 0; k < m_c; k++) if (!k || mcn[k] != mcn[k - 1]) { bu32(&b, k + 1); bu32(&b, mcn[k]); bu32(&b, 1); }
          aend(&b, o); }
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 20); bu32(&b, m_n); aend(&b, o);
        put_co64(&b, moff[t], m_c);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_gyro) {                       /* Gyroflow Protobuf telemetry: one Main per frame */
        uint32_t tid = 2 + have_tc + have_wav + (m_c ? 2 : 0);
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0x3); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "cdsc"); bu32(&b, 1); aend(&b, t2); aend(&b, tf);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, fps_n); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "meta", "GyroflowTelemetry");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "nmhd"); bu32(&b, 0); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Apple Alias Data Handler"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "gyrf"); bzero_(&b, 6); bu16(&b, 1); aend(&b, en);
        aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, nf); bu32(&b, fps_d); aend(&b, o);
        put_stsc_ones(&b);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 0); bu32(&b, nf); for (int i = 0; i < nf; i++) bu32(&b, (uint32_t)glen[i]); aend(&b, o);
        put_co64(&b, goff, nf);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    double secs = (double)nf * fps_d / fps_n, bitrate = secs > 0 ? 8.0 * (double)(mdat_end - 4100) / secs : 0;
    Meta *meta = calloc(1, sizeof(Meta)); clip_meta(meta, &first, &col, &L, codec, clipname, bitrate); put_meta(&b, meta);
    aend(&b, moov);
    fwrite(b.p, 1, b.n, fo);
    if (fclose(fo)) { unlink(partial); die("error writing %s", partial); }
    if (rename(partial, out)) die("can't rename to %s", out);
    struct stat sb; stat(out, &sb);
    printf("wrote %s  (%.1f MB, %s)\n", out, sb.st_size / 1e6, codec == BRAW_CODEC_STAGE ? "staging: uncompressed slices" : "Blackmagic RAW");

    if (verify && codec == BRAW_CODEC_STAGE) {         /* read every frame back and compare with the DNG */
        FILE *fi = fopen(out, "rb"); uint8_t *buf = NULL; uint16_t *back = malloc((size_t)first.w * first.h * 2); long bad = 0;
        for (int i = 0; i < nf; i++) {
            buf = realloc(buf, vsz[i]); fseeko(fi, (off_t)voff[i], SEEK_SET);
            if (fread(buf, 1, vsz[i], fi) != vsz[i]) die("%s", "verify: short read");
            if (braw_stage_decode(buf, vsz[i], &L, back, err, sizeof err)) die("verify: %s", err);
            Frame f; dngf_load_frame(files[i], &f, 1);
            if (memcmp(f.raw, back, (size_t)f.w * f.h * 2)) { bad++; fprintf(stderr, "verify: frame %d differs\n", i); }
            dngf_free_frame(&f);
        }
        fclose(fi); free(buf); free(back);
        if (bad) { fprintf(stderr, "verify: %ld frame(s) differ\n", bad); return 4; }
        printf("verify: all %d frame(s) read back bit-exact\n", nf);
    }
    return 0;
}
