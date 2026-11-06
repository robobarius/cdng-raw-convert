/* braw_wb.h: Blackmagic-style as-shot kelvin/tint from a DNG AsShotNeutral (FINDINGS 37). dng2braw, LGPL-2.1-or-later. */
#ifndef BRAW_WB_H
#define BRAW_WB_H
/* neutral: DNG AsShotNeutral (camera R, G, B). Returns 0 and kelvin/tint, or -1 outside the measured range. */
int braw_wb_from_neutral(const double neutral[3], int *kelvin, int *tint);
#endif
