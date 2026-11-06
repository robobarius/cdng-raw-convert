/*
 * braw_format.h: the Blackmagic RAW layout that a Video Assist 12G writes for a SIGMA fp over
 * HDMI (docs/FINDINGS.md sections 7-11), and the dng2braw frame builder.
 *
 * Part of dng2braw (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * NOT Blackmagic software. Everything here comes from the files themselves (tools/braw_explore.py);
 * fields marked "observed" are copied without knowing their meaning.
 *
 * Two codecs share the layout:
 *   BRAW_CODEC_STAGE  "fpbr": the full layout with each slice stored uncompressed. Exercises
 *                     everything except the compression, reads back bit for bit, and cannot be
 *                     mistaken for real BRAW (different codec and frame magic, .fpbraw).
 *   BRAW_CODEC_BRAW   "brvl": real Blackmagic RAW, 5:1 profile (source/braw_encode.c; FINDINGS 21-30).
 */
#ifndef BRAW_FORMAT_H
#define BRAW_FORMAT_H
#include <stdint.h>
#include "qt_writer.h"

typedef enum { BRAW_CODEC_STAGE = 0, BRAW_CODEC_BRAW = 1, BRAW_CODEC_SPLICE = 2 } BrawCodec;
/* BRAW_CODEC_SPLICE (developer test): real Blackmagic RAW whose slices (and quantiser header words
   0x08-0x3f) are copied from a reference frame recorded by a Video Assist. Exercises dng2braw's
   container, metadata, frame header and slice table against the real decoder, before the slice
   encoder exists. The picture is the reference's, not the DNG's. */
int braw_set_splice(const uint8_t *payload, size_t len, char *err, size_t errlen);

#define BRAW_MAX_COLS 8
#define BRAW_BMDF_SIZE 256            /* per-frame metadata box (Video Assist; camera files use 1024) */
#define BRAW_FRAME_ALIGN 256          /* Video Assist frames are multiples of 256 bytes */
#define BRAW_HEADER_SIZE 4608         /* first slice offset (0x1200), all recorders seen */
#define BRAW_STRIPS 5                 /* Video Assist + fp: 5 strips (768 x4 + 784 at 3856 wide) */
#define BRAW_SLICE_LINES 72           /* frame header 0x10: slice height in lines */
#define BRAW_PAD_TOP 2                /* coded row = DNG row + 2 (FINDINGS 23: DNG-derived planes processed by the
                                         SDK line up with the Video Assist decode, and only an even shift keeps
                                         the CFA phase; the earlier value 3 came from render cross-correlation). */

/* how a frame is cut into slices: vertical strips x horizontal bands, column-major */
typedef struct {
    int w, h;                         /* DNG image size */
    int coded_w, coded_h;             /* coded frame size (3856 x 2176 for fp UHD) */
    int pad_top;                      /* lines added above the DNG image */
    int ncols, col_w[BRAW_MAX_COLS];
    int nrows, row_h;                 /* bands of row_h lines; the last one is shorter */
} BrawLayout;
int braw_layout(int w, int h, BrawLayout *L);
/* coded sizes whose BRAW output has been checked through Blackmagic's decoder (FINDINGS 40) */
int braw_size_tested(int coded_w, int coded_h);
/* anamorphic: a 'pasp' box (horizontal:vertical pixel spacing) in the sample entry; 1:1 writes none */
void braw_set_pixel_aspect(uint32_t h, uint32_t v);
void braw_set_rate_denominator(uint32_t d);
void braw_normalise_rate(int *n, int *d);
/* "2", "1.33", "1.6x" ... -> pixel aspect as a small fraction (4:3 for 1.33, 5:3 for 1.66); 0 on a bad value */
int braw_parse_desqueeze(const char *s, uint32_t *h, uint32_t *v, double *f);

/* per-frame values carried in the 'bmdf' box (Video Assist order) */
typedef struct {
    int fps_n, fps_d;                 /* 'srte' */
    double shutter_angle;             /* 'shtv', written as text, e.g. "287.7°" */
    double analog_gain;               /* 'agpf' */
    int kelvin, tint;                 /* 'asct'/'asti' and 'wkel'/'wtin' */
    double fnumber;                   /* 'aptr', e.g. "F2.8" */
    double focal_mm;                  /* 'fcln', e.g. "85mm" */
    int iso;                          /* 'isoe' */
} BrawFrameMeta;

/* Builds one frame sample (bmdf box + image payload, padded to 256) into out.
   raw: L->w x L->h photosites (stride w), the DNG's stored values; bits: their depth; black: DNG black level.
   Returns 0, or -1 with a message in err. */
int braw_build_frame(const BrawLayout *L, BrawCodec codec, const uint16_t *raw, int bits, double black,
                     const BrawFrameMeta *fm, Buf *out, char *err, size_t errlen);

/* Reads a BRAW_CODEC_STAGE frame back into raw (L->w x L->h, padding dropped). 0 = ok. */
int braw_stage_decode(const uint8_t *frame, size_t len, const BrawLayout *L, uint16_t *raw, char *err, size_t errlen);

/* The slice coder. coded: L->coded_w x L->coded_h. For BRAW_CODEC_BRAW this is the real bitstream. */
int braw_encode_slice(BrawCodec codec, const uint16_t *coded, int stride, int x0, int y0, int w, int h,
                      int bits, Buf *out, char *err, size_t errlen);

/* Rate control for BRAW_CODEC_BRAW (FINDINGS 32): mode 0 = constant quality, value = matrix scale (1 = the Video
   Assist's flat 33); mode 1 = target compression ratio (value = N for N:1, against 12-bit coded size). */
void braw_set_rate(int mode, double value);
double braw_last_scale(void);
/* GPU pipeline (macOS Metal builds; FINDINGS 35). braw_prepare_frame() starts a frame's transform + DCT on the GPU;
   the next braw_build_frame() for BRAW_CODEC_BRAW uses the oldest prepared frame instead of computing it. Frames must
   be prepared and built in the same order, at most BRAW_GPU_SLOTS (2) ahead. Returns -1 when the GPU isn't used. */
int braw_prepare_frame(const BrawLayout *L, const uint16_t *raw, double black);
void braw_set_gpu(int on);
const char *braw_gpu_status(const BrawLayout *L);                /* matrix scale used for the last frame */

/* 'brvl' (or 'fpbr') sample entry, 122 bytes as the Video Assist writes it */
void braw_put_sample_entry(Buf *b, BrawCodec codec, int w, int h);

#endif
