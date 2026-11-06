/*
 * braw_wb.c: as-shot white balance for the clip/frame metadata, the way Blackmagic's processing for camera type
 * "SIGMA fp L" interprets it (FINDINGS 37). The table holds, for each (kelvin, tint), the camera-raw neutral
 * (ln R/G, ln B/G of photosite values) that Blackmagic's processing renders neutral. It was measured black-box with
 * the public SDK API (tools/wb_table_probe.py): flat planes made by dng2braw's own forward transform (DNG-scaled, i.e. without
 * the raw gains of FINDINGS 38: that version reproduces the Video Assist's own numbers best), processed with --wb K T. Regenerate with tools/wb_table_to_c.py. braw_wb_from_neutral() inverts it for a DNG AsShotNeutral. Part of dng2braw (fpSup community).
 * LGPL-2.1-or-later. NOT Blackmagic software.
 */
#include <math.h>
#include "braw_wb.h"
#define NK 12
#define NT 6
static const double KS[NK] = {2500, 2800, 3200, 3600, 4000, 4500, 5000, 5600, 6300, 7200, 8500, 10000};
static const double TS[NT] = {-40, -20, 0, 20, 40, 60};
static const double TAB[NK][NT][2] = {
    /*  2500 K */ { {0.16739, -0.48024}, {0.11349, -0.89346}, {0.06354, -1.50213}, {0.01439, -2.84190}, {NAN, NAN}, {0.01439, -2.84190} },
    /*  2800 K */ { {0.01521, -0.33334}, {-0.04039, -0.66108}, {-0.09740, -1.09452}, {-0.15024, -1.74571}, {NAN, NAN}, {NAN, NAN} },
    /*  3200 K */ { {-0.09922, -0.25847}, {-0.16233, -0.51717}, {-0.22526, -0.82407}, {-0.28763, -1.22510}, {-0.34244, -1.80116}, {-0.39902, -3.01350} },
    /*  3600 K */ { {-0.17762, -0.21071}, {-0.25221, -0.42829}, {-0.32184, -0.67024}, {-0.38651, -0.95226}, {-0.45535, -1.31641}, {-0.52147, -1.81283} },
    /*  4000 K */ { {-0.23644, -0.18169}, {-0.31882, -0.36532}, {-0.39543, -0.57110}, {-0.47565, -0.79293}, {-0.55127, -1.06311}, {-0.62521, -1.38229} },
    /*  4500 K */ { {-0.29444, -0.15851}, {-0.38344, -0.31547}, {-0.47110, -0.48730}, {-0.56291, -0.66710}, {-0.64955, -0.86779}, {-0.73518, -1.09250} },
    /*  5000 K */ { {-0.33803, -0.14961}, {-0.43630, -0.28885}, {-0.53210, -0.43802}, {-0.62777, -0.58913}, {-0.72819, -0.75546}, {-0.82981, -0.93210} },
    /*  5600 K */ { {-0.37567, -0.15026}, {-0.48203, -0.27770}, {-0.58824, -0.40543}, {-0.69702, -0.53563}, {-0.80780, -0.67397}, {-0.92415, -0.81615} },
    /*  6300 K */ { {-0.41013, -0.16508}, {-0.52289, -0.27882}, {-0.63715, -0.39151}, {-0.76097, -0.50457}, {-0.88542, -0.62322}, {-1.01272, -0.74388} },
    /*  7200 K */ { {-0.45445, -0.10071}, {-0.58287, -0.19829}, {-0.70878, -0.29852}, {-0.84196, -0.40188}, {-0.98059, -0.50134}, {-1.12914, -0.60080} },
    /*  8500 K */ { {-0.50954, 0.00739}, {-0.64261, -0.08379}, {-0.78449, -0.17802}, {-0.93532, -0.26229}, {-1.09193, -0.34847}, {-1.26509, -0.43606} },
    /* 10000 K */ { {-0.55335, 0.09375}, {-0.69495, 0.00855}, {-0.85505, -0.07196}, {-1.01289, -0.15151}, {-1.18573, -0.23083}, {-1.38143, -0.30738} },
};
static int lookup(double mired, double tint, double *lr, double *lb) {   /* bilinear in (mired, tint) */
    double m[NK]; for (int i = 0; i < NK; i++) m[i] = 1e6 / KS[i];        /* mired decreases along KS */
    if (mired > m[0] || mired < m[NK - 1] || tint < TS[0] || tint > TS[NT - 1]) return -1;
    int i = 0; while (i < NK - 2 && mired < m[i + 1]) i++;
    int j = 0; while (j < NT - 2 && tint > TS[j + 1]) j++;
    double u = (m[i] - mired) / (m[i] - m[i + 1]), v = (tint - TS[j]) / (TS[j + 1] - TS[j]);
    for (int c = 0; c < 2; c++) {
        double a = TAB[i][j][c], b = TAB[i + 1][j][c], d = TAB[i][j + 1][c], e = TAB[i + 1][j + 1][c];
        if (isnan(a) || isnan(b) || isnan(d) || isnan(e)) return -1;
        double val = (1 - u) * (1 - v) * a + u * (1 - v) * b + (1 - u) * v * d + u * v * e;
        if (c == 0) *lr = val; else *lb = val;
    }
    return 0;
}
int braw_wb_from_neutral(const double neutral[3], int *kelvin, int *tint) {
    if (!(neutral[0] > 0 && neutral[1] > 0 && neutral[2] > 0)) return -1;
    double tr = log(neutral[0] / neutral[1]), tb = log(neutral[2] / neutral[1]);
    double best = 1e9, bm = 0, bt = 0;
    for (double mi = 1e6 / KS[NK - 1]; mi <= 1e6 / KS[0]; mi += 0.5)      /* fine search, then refine */
        for (double t = TS[0]; t <= TS[NT - 1]; t += 0.5) {
            double lr, lb; if (lookup(mi, t, &lr, &lb)) continue;
            double e = (lr - tr) * (lr - tr) + (lb - tb) * (lb - tb); if (e < best) { best = e; bm = mi; bt = t; }
        }
    for (double mi = bm - 0.5; mi <= bm + 0.5; mi += 0.02)
        for (double t = bt - 0.5; t <= bt + 0.5; t += 0.02) {
            double lr, lb; if (lookup(mi, t, &lr, &lb)) continue;
            double e = (lr - tr) * (lr - tr) + (lb - tb) * (lb - tb); if (e < best) { best = e; bm = mi; bt = t; }
        }
    if (best > 0.01) return -1;                                          /* neutral outside the measured range */
    *kelvin = (int)lround(1e6 / bm); *tint = (int)lround(bt);
    return 0;
}
