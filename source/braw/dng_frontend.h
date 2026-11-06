/*
 * dng_frontend.h: the SIGMA fp CinemaDNG reader shared by the dng2* encoders.
 *
 * Part of dng2braw / dng2prr (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Extracted unchanged from dng2prr 0.2's dng2prr_cli.c (TIFF/DNG reading + colour maths),
 * so dng2prr can move onto this file when the three outputs are merged (docs/INTEGRATION.md).
 */
#ifndef DNG_FRONTEND_H
#define DNG_FRONTEND_H
#include <stdint.h>

typedef struct {
    int w, h, bits;
    uint16_t *raw;                 /* h x w */
    double black, white, cm1[9], cm2[9], neutral[3], baseline_exposure;
    int has_cm1, illum1, illum2;
    int fps_n, fps_d, crop[4];     /* l r t b */
    int iso; double fnumber, exposure_time, focal; char lens[256];
    uint16_t *lin; int nlin;       /* LinearizationTable, kept when loaded with pixels == 2 */
    uint8_t tc[8]; int has_tc;     /* DNG TimeCodes (SMPTE 12M, BCD) */
} Frame;

typedef struct { double wb_r, wb_b, M[9]; int cct; } Colour;

/* pixels: 0 = tags only, 1 = decode + apply LinearizationTable, 2 = decode, keep the table */
int dngf_load_frame(const char *path, Frame *f, int pixels);
void dngf_free_frame(Frame *f);
/* as-shot white balance and camera->XYZ(D65) matrix, CCT, from the DNG's own calibration */
Colour dngf_colour_params(const Frame *f);
/* white-balance multipliers + Planckian neutral for a temperature (for tint estimation) */
void dngf_locus_neutral(const Frame *f, double T, double *n);

#endif
