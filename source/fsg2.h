/*
 * fsg2.h: fpSup Gyro2 (FSG2 kind 2) telemetry -> Gyroflow Protobuf.
 * Reads both carriers and both payload versions the Gyroflow (fpSup) plugin v0.1.10 reads.
 * GPL-3.0-or-later (a port of ijigen/gyroflow-plugins common/src/fsg2.rs and lensfit.rs).
 */
#ifndef FSG2_H
#define FSG2_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint16_t pos; uint8_t kind, len; uint8_t v[8]; } Fsg2Event;

typedef struct {
    int present;                 /* 1 = a valid kind-2 block was found */
    uint16_t flags; uint32_t clip_id, frame_seq, period_ps; float gscale, ascale;
    uint32_t readout_ns; uint16_t mode_id, crop[4], frame_mark; uint32_t base_idx;
    int n_samples, n_events; int16_t (*samples)[3]; Fsg2Event *events;
    int has_lens; uint32_t calib_focal; uint32_t axis[5]; double nodes[5][3][4];
    int fmt_version;             /* payload format: 1, or 2 (fpSup Gyro2 v0.2.0 and later) */
    int carrier;                 /* 1 = block in the MakerNote's zero tail, 2 = Gyro2's own SIGMA MakerNote tag 0x4653 */
    uint32_t calib2;             /* version 2: the frame's calibrated focal length, tenths of a mm (0 = none) */
    uint16_t hmax, read_w, lines, bin_h, bin_v;   /* version 2: the sensor mode as read */
    /* the frame's own DNG/EXIF values (0 / "" = absent) */
    char make[64], model[64], serial[64], software[96], lens_model[128];
    double fps, exposure_s, f_number, focal_mm, distance_m; int iso;
    int has_warp; double warp[4];
} Fsg2Frame;

/* Read one DNG's FSG2 block and EXIF values. Returns 0 with f->present = 1 on success,
   0 with f->present = 0 when the frame has no (kind 2) block, -1 on a broken block. */
int fsg2_read(const char *path, Fsg2Frame *f, char *err, size_t errlen);
void fsg2_free(Fsg2Frame *f);

/* Turn a take's frames (file order) into one serialized Gyroflow Main message per frame.
   width/height: the picture the decoder will show. Returns 0 or -1 with err set.
   msgs[i] / lens[i] are malloc'd. If jsonl is non-NULL, the same messages are also written
   to it as Gyroflow JSONL (one Main per line). */
int fsg2_messages(Fsg2Frame *fr, int n, int width, int height, uint8_t ***msgs, size_t **lens,
                  void *jsonl_file, char *err, size_t errlen);
/* The readings of the take on the camera clock, as fsg2_messages writes them (gyro deg/s, accelerometer m/s²,
   message axes), and each frame's start_timestamp_us. */
typedef struct { long n; double *ts_us; float (*gyro)[3], (*accel)[3]; double *frame_start_us, fps, readout_us, exposure_us; } Fsg2Imu;
int fsg2_imu(Fsg2Frame *fr, int n, Fsg2Imu *out, char *err, size_t errlen);
void fsg2_imu_free(Fsg2Imu *m);
#endif
