/* Unit tests for source/fsg2.c, ported from the Rust tests in ijigen/gyroflow-plugins
 * common/src/fsg2.rs and lensfit.rs (same golden values). GPL-3.0-or-later.
 *   cc -O2 -ffp-contract=off -o test_fsg2 tests/test_fsg2.c -lm && ./test_fsg2 */
#include "../source/fsg2.c"
#include <assert.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t AXIS[5] = { 0, 18641, 37283, 49637, 55924 };
static const double GREEN[5][4] = {
    {0.987878098, 0.000230792, -0.001804660, 0.012710940}, {0.995612771, -0.003803673, -0.003186222, 0.011056404},
    {0.999854447, -0.010596089, 0.003693395, 0.003874420}, {0.999856513, -0.016680599, 0.013089761, -0.003420052},
    {0.999845119, -0.020184787, 0.018855619, -0.007281209} };
static const struct { double d, kr[4], k[3], fx; } GOLDEN[6] = {
    {INFINITY, {9.878780980000000e-01, 2.307920000000000e-04, -1.804660000000000e-03, 1.271094000000000e-02}, {3.434034356654717e-01, -9.580878466073323e-02, 1.931069370926159e+00}, 2130.954872120334},
    {900.0, {9.956128508894739e-01, -3.803800931162369e-03, -3.186092426458057e-03, 1.105626873155336e-02}, {3.266573683372537e-01, -9.813155189668302e-02, 1.595721472338275e+00}, 2147.639531278018},
    {450.0, {9.998543792456360e-01, -1.059598050141926e-02, 3.693285108515062e-03, 3.874534721340819e-03}, {2.966047906818278e-01, 7.396930653660273e-02, 7.564556456382933e-01}, 2156.788945091422},
    {364.0, {9.998559200330400e-01, -1.493427112102856e-02, 1.039289043740427e-02, -1.326450409078700e-03}, {2.772084741693018e-01, 2.200148386305785e-01, 2.240453137977921e-01}, 2156.792268728652},
    {300.0, {9.998451189999999e-01, -2.018478700000000e-02, 1.885561900000000e-02, -7.281209000000000e-03}, {2.540190304919543e-01, 3.954371794339360e-01, -3.550155534415396e-01}, 2156.768969787187},
    {250.0, {9.998451189999999e-01, -2.018478700000000e-02, 1.885561900000000e-02, -7.281209000000000e-03}, {2.540190304919543e-01, 3.954371794339360e-01, -3.550155534415396e-01}, 2156.768969787187} };

int main(void) {
    double nodes[5][3][4]; for (int i = 0; i < 5; i++) for (int p = 0; p < 3; p++) memcpy(nodes[i][p], GREEN[i], sizeof GREEN[i]);
    double focal = lf_focal_px(1936, 40.0, 6000);
    CHECK(fabs(focal - 2157.1030640668523) < 1e-9, "focal_px %f", focal);
    for (int g = 0; g < 6; g++) {
        double kr[4], k[4], fx;
        CHECK(lf_interpolate(AXIS, nodes, GOLDEN[g].d, kr), "interpolate %g", GOLDEN[g].d);
        for (int i = 0; i < 4; i++) CHECK(fabs(kr[i] - GOLDEN[g].kr[i]) < 1e-12, "%g mm kr%d %.15g vs %.15g", GOLDEN[g].d, i, kr[i], GOLDEN[g].kr[i]);
        CHECK(lf_fit(kr, 1936, 1090, focal, k, &fx), "fit");
        for (int i = 0; i < 3; i++) CHECK(fabs(k[i] - GOLDEN[g].k[i]) < 1e-8, "%g mm k%d %.15g vs %.15g", GOLDEN[g].d, i, k[i], GOLDEN[g].k[i]);
        CHECK(k[3] == 0 && fabs(fx - GOLDEN[g].fx) < 1e-8, "fx %.12f", fx);
    }
    CHECK(lf_focal_mm(1, 394, 40.0) == 39.4 && lf_focal_mm(1, 0, 40.0) == 40.0 && lf_focal_mm(1, 900, 40.0) == 40.0 &&
          lf_focal_mm(0, 0, 28.0) == 28.0 && lf_focal_mm(1, 280, NAN) == 28.0 && lf_focal_mm(0, 0, NAN) == 0, "focal_mm rule");
    { double z[5][3][4] = {{{0}}}, kr[4]; CHECK(!lf_interpolate(AXIS, z, NAN, kr), "no data"); }

    /* each_frame_takes_its_own_vd_through_late_hooks_and_skipped_frames */
    {
        const uint32_t PERIOD_PS = 400085400;   /* as the Rust test */
        double fps = 30000.0 / 1001.0, per_frame = 1.0 / (fps * PERIOD_PS * 1e-12);
        #define VD(n) (1000.0 + (n) * per_frame * (1.0 + 2e-3))
        int N = 40; double sensor[40]; uint32_t hooks[40]; double lag[5] = { 35, 34, 36, 35, 35 };
        for (int k = 0; k < N; k++) { sensor[k] = k < 25 ? k : k + 1; hooks[k] = (uint32_t)round(VD(sensor[k]) + lag[k % 5] + (k == 12 ? 60.0 : 0.0)); }
        uint32_t vds[41]; int nv = 0; for (int n = 0; n < 41; n++) if (n % 7 != 3) vds[nv++] = (uint32_t)round(VD((double)n));
        Fsg2Frame fr[40]; memset(fr, 0, sizeof fr); uint32_t start = 900;
        for (int k = 0; k < N; k++) {
            fr[k].present = 1; fr[k].period_ps = PERIOD_PS; fr[k].frame_seq = k; fr[k].base_idx = start; fr[k].frame_mark = (uint16_t)(hooks[k] - start);
            fr[k].events = calloc(8, sizeof(Fsg2Event));
            for (int v = 0; v < nv; v++) if (vds[v] >= start && vds[v] < hooks[k]) { Fsg2Event *e = &fr[k].events[fr[k].n_events++]; e->pos = (uint16_t)(vds[v] - start); e->kind = EVENT_VD; e->len = 4; }
            start = hooks[k];
        }
        double got[40]; char err[200];
        CHECK(vd_frame_positions(fr, N, fps, got, err, sizeof err) == 0, "vd: %s", err);
        for (int k = 0; k < N; k++) CHECK(fabs(got[k] - VD(sensor[k])) < 0.6, "frame %d: %f vs %f", k, got[k], VD(sensor[k]));
        for (int k = 0; k < N; k++) fr[k].n_events = 0;
        CHECK(vd_frame_positions(fr, N, fps, got, err, sizeof err) && strstr(err, "no Vd"), "no Vd refused");
        for (int k = 0; k < N; k++) free(fr[k].events);
    }
    /* payload version 2 (fsg2.rs version_2_round_trip_and_offsets, version_2_derives_the_window_like_gyro1,
       version_2_has_no_lens_table_even_with_the_flag) */
    {
        uint8_t p[HEADER_BYTES + 6 + 10]; memset(p, 0, sizeof p);
        p[0] = 2; p[12] = 0x18; p[13] = 0x2A; p[14] = 0xD8; p[15] = 0x17;        /* sample period 400085400 ps */
        float one = 1.0f; memcpy(p + 16, &one, 4); memcpy(p + 20, &one, 4);
        const uint8_t mode[14] = { 123, 0, 77, 1, 0xF0, 0x16, 0xE8, 0x0C, 1, 0, 1, 0, 0x8A, 1 };   /* as the Rust test's bytes */
        memcpy(p + 24, mode, 14); p[38] = 2; p[40] = 0xDD; p[41] = 0xCC; p[42] = 0xBB; p[43] = 0xAA; p[44] = 1; p[46] = 1;
        p[48] = 1; p[50] = 0xFE; p[51] = 0xFF; p[52] = 3;                             /* sample [1, -2, 3] */
        p[54] = 0; p[56] = EVENT_LEVEL; p[57] = 6; p[58] = 10; p[60] = 0xEC; p[61] = 0xFF; p[62] = 0x00; p[63] = 0x04;   /* level [10, -20, 1024] */
        Fsg2Frame f; memset(&f, 0, sizeof f); char err[200] = "";
        CHECK(parse_payload(p, sizeof p, &f, err, sizeof err) == 0, "v2 parse: %s", err);
        CHECK(f.fmt_version == 2 && f.mode_id == 123 && f.hmax == 333 && f.read_w == 5872 && f.lines == 3304 && f.calib2 == 394, "v2 header fields");
        CHECK(f.readout_ns == 15281000, "v2 readout %u", f.readout_ns);
        CHECK(f.crop[0] == 64 && f.crop[1] == 348 && f.crop[2] == 5872 && f.crop[3] == 3304, "v2 crop %u %u %u %u", f.crop[0], f.crop[1], f.crop[2], f.crop[3]);
        CHECK(f.frame_mark == 2 && f.base_idx == 0xAABBCCDDu && f.n_samples == 1 && f.samples[0][1] == -2 && f.n_events == 1, "v2 body");
        fsg2_free(&f);
        /* mode 106 (Gyro1's A001_040): 3000 x 2000 read at 2 x 2, hmax 380 */
        const uint8_t m106[12] = { 106, 0, 0x7C, 1, 0xB8, 0x0B, 0xD0, 0x07, 2, 0, 2, 0 }; memcpy(p + 24, m106, 12); p[36] = p[37] = 0;
        memset(&f, 0, sizeof f); CHECK(parse_payload(p, sizeof p, &f, err, sizeof err) == 0, "m106: %s", err);
        CHECK(f.readout_ns == 10555556 && f.crop[0] == 0 && f.crop[1] == 0 && f.crop[2] == 6000 && f.crop[3] == 4000 && f.calib2 == 0, "m106 %u", f.readout_ns); fsg2_free(&f);
        /* binning 0 is 1; more than the sensor is the sensor */
        const uint8_t odd[12] = { 1, 0, 1, 0, 0x58, 0x1B, 100, 0, 0, 0, 0, 0 }; memcpy(p + 24, odd, 12);
        memset(&f, 0, sizeof f); CHECK(parse_payload(p, sizeof p, &f, err, sizeof err) == 0, "odd: %s", err);
        CHECK(f.crop[0] == 0 && f.crop[1] == 1950 && f.crop[2] == 6000 && f.crop[3] == 100, "odd crop %u %u %u %u", f.crop[0], f.crop[1], f.crop[2], f.crop[3]); fsg2_free(&f);
        /* no mode running: zeros, as version 1 wrote them */
        p[24] = 0; memset(&f, 0, sizeof f); CHECK(parse_payload(p, sizeof p, &f, err, sizeof err) == 0 && f.readout_ns == 0 && f.crop[2] == 0, "mode 0"); fsg2_free(&f);
        /* the lens-table flag means nothing in version 2 */
        p[24] = 123; p[2] = FLAG_LENS_TABLE; memset(&f, 0, sizeof f);
        CHECK(parse_payload(p, sizeof p, &f, err, sizeof err) == 0 && !f.has_lens, "v2 flagged: %s", err); fsg2_free(&f);
    }
    printf(fails ? "%d FAILED\n" : "all fsg2 tests passed\n", fails);
    return fails != 0;
}
