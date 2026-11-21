#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- 第 1 段：按块取峰值 ---- */
static void test_meter_peaks(void) {
    enum { FRAMES = 10, CH = 2, BLOCK = 4 };
    float src[FRAMES * CH];
    for (int i = 0; i < FRAMES; ++i) {
        src[i * 2 + 0] = (float)i * 0.1f;
        src[i * 2 + 1] = -(float)i * 0.05f;
    }
    float peaks[3 * CH];
    int nb = -1;
    CK(mx_meter_peaks(src, FRAMES, CH, BLOCK, peaks, &nb) == 3);
    CK(nb == 3);
    /* 块 0 = 帧 0..3，块 1 = 4..7，块 2 = 8..9（不足一块） */
    CK(fabsf(peaks[0] - 0.3f) < 1e-6f && fabsf(peaks[1] - 0.15f) < 1e-6f);
    CK(fabsf(peaks[2] - 0.7f) < 1e-6f && fabsf(peaks[3] - 0.35f) < 1e-6f);
    CK(fabsf(peaks[4] - 0.9f) < 1e-6f && fabsf(peaks[5] - 0.45f) < 1e-6f);
}

/* ---- 手算包络：0.75 的单峰，块 4 帧、保持 8 帧、每帧回落 0.25 ---- */
static void test_meter_hand_envelope(void) {
    enum { FRAMES = 16, BLOCK = 4, HOLD = 8 };
    float src[FRAMES];
    for (int i = 0; i < FRAMES; ++i) src[i] = 0.0f;
    src[1] = 0.75f;

    float out[FRAMES];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.25f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, &rep) == FRAMES);

    /* 触发帧 = 块首（帧 0），保持 8 帧（0..7），随后每帧 -0.25：
     * 8:0.50 9:0.25 10:0.00 之后一直为 0 */
    float expect[FRAMES] = { 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f,
                             0.50f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < FRAMES; ++i) CK(fabsf(out[i] - expect[i]) < 1e-6f);

    CK(rep.frames == FRAMES && rep.channels == 1 && rep.blocks == 4);
    CK(fabsf(rep.peak_in - 0.75f) < 1e-6f);
    CK(fabsf(rep.peak_out - 0.75f) < 1e-6f);
    CK(rep.peak_err < 1e-6f);           /* 峰值误差为 0 */
    CK(rep.hold_observed == HOLD);      /* 实测保持帧数 = 配置值 */
    CK(rep.overrun_blocks == 0 && rep.overrun_runs == 0 && rep.recovery_frames == 0);
}

/* ---- 等幅新峰在保持到点后重新顶住 ---- */
static void test_meter_retrigger(void) {
    enum { FRAMES = 20, BLOCK = 4, HOLD = 4 };
    float src[FRAMES];
    for (int i = 0; i < FRAMES; ++i) src[i] = 0.0f;
    src[1] = 0.75f;
    src[8] = 0.75f;

    float out[FRAMES];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.25f, 1 };
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == FRAMES);

    /* 帧 0..3 保持 0.75；4..7 回落 0.50/0.25/0/0；帧 8 等幅新峰重新顶住 4 帧 */
    float expect[FRAMES] = { 0.75f, 0.75f, 0.75f, 0.75f,
                             0.50f, 0.25f, 0.0f, 0.0f,
                             0.75f, 0.75f, 0.75f, 0.75f,
                             0.50f, 0.25f, 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < FRAMES; ++i) CK(fabsf(out[i] - expect[i]) < 1e-6f);
}

/* ---- 关闭保持：只输出块峰值逐帧展开，与 hold/release 取值无关 ---- */
static void test_meter_bypass(void) {
    enum { FRAMES = 12, CH = 2, BLOCK = 4 };
    float src[FRAMES * CH];
    for (int i = 0; i < FRAMES * CH; ++i) src[i] = 0.01f * (float)i - 0.1f;
    src[2 * CH + 1] = -0.8f;   /* 块 0 声道 1 */
    src[9 * CH + 0] = 0.6f;    /* 块 2 声道 0 */

    /* 旧输出 = 块峰值逐帧展开 */
    float peaks[3 * CH];
    int nb = 0;
    CK(mx_meter_peaks(src, FRAMES, CH, BLOCK, peaks, &nb) == 3);
    float expect[FRAMES * CH];
    for (int i = 0; i < FRAMES; ++i)
        for (int c = 0; c < CH; ++c)
            expect[i * CH + c] = peaks[(i / BLOCK) * CH + c];

    float out_a[FRAMES * CH], out_b[FRAMES * CH];
    MxMeterCfg a = { BLOCK, 1, 0.0f, 0 };
    MxMeterCfg b = { BLOCK, 500, 7.0f, 0 };
    CK(mx_meter_process(src, FRAMES, CH, &a, out_a, NULL) == FRAMES);
    CK(mx_meter_process(src, FRAMES, CH, &b, out_b, NULL) == FRAMES);
    CK(memcmp(out_a, out_b, sizeof(out_a)) == 0);
    for (int i = 0; i < FRAMES * CH; ++i) CK(out_a[i] == expect[i]);
    /* 峰值同样不丢失 */
    float pin = 0.0f, pout = 0.0f;
    for (int i = 0; i < FRAMES * CH; ++i) {
        float x = fabsf(src[i]), y = fabsf(out_a[i]);
        if (x > pin) pin = x;
        if (y > pout) pout = y;
    }
    CK(fabsf(pin - pout) < 1e-6f);
}

/* ---- 保持为零、回落为负：报错并指出字段，且不写输出 ---- */
static void test_meter_errors(void) {
    enum { FRAMES = 8 };
    float src[FRAMES], out[FRAMES];
    for (int i = 0; i < FRAMES; ++i) { src[i] = 0.1f; out[i] = -123.0f; }

    MxMeterCfg cfg = { 4, 0, 0.1f, 1 };
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == MX_METER_EHOLD);
    CK(strcmp(mx_meter_field(MX_METER_EHOLD), "hold_frames") == 0);
    for (int i = 0; i < FRAMES; ++i) CK(out[i] == -123.0f);

    cfg.hold_frames = -3;
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == MX_METER_EHOLD);

    cfg.hold_frames = 4; cfg.release = -0.01f;
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == MX_METER_ERELEASE);
    CK(strcmp(mx_meter_field(MX_METER_ERELEASE), "release") == 0);
    for (int i = 0; i < FRAMES; ++i) CK(out[i] == -123.0f);

    cfg.release = NAN;
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == MX_METER_ERELEASE);

    cfg.release = 0.0f;
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == FRAMES);

    cfg.block_frames = 0;
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == MX_METER_EBLOCK);
    CK(strcmp(mx_meter_field(MX_METER_EBLOCK), "block_frames") == 0);

    cfg.block_frames = 4;
    cfg.hold_frames = 2;
    CK(mx_meter_process(src, FRAMES, 0, &cfg, out, NULL) == MX_METER_ECHANNELS);
    CK(strcmp(mx_meter_field(MX_METER_ECHANNELS), "channels") == 0);
    CK(mx_meter_process(src, 0, 1, &cfg, out, NULL) == MX_METER_EARG);
    CK(strcmp(mx_meter_field(MX_METER_EARG), "frames") == 0);
    CK(mx_meter_process(NULL, FRAMES, 1, &cfg, out, NULL) == MX_METER_EARG);
    CK(mx_meter_process(src, FRAMES, 1, NULL, out, NULL) == MX_METER_EARG);

    /* 单段函数也各自校验对应字段 */
    float peaks[2], hl[4];
    int left[4], nb = 0;
    CK(mx_meter_peaks(src, FRAMES, 1, 0, peaks, &nb) == MX_METER_EBLOCK);
    CK(mx_meter_peaks(NULL, FRAMES, 1, 4, peaks, &nb) == MX_METER_EARG);
    CK(mx_meter_hold(peaks, 4, 1, 4, 0, hl, left) == MX_METER_EHOLD);
    CK(mx_meter_release(peaks, hl, left, 4, 1, 4, 4, -1.0f, out) == MX_METER_ERELEASE);
}

/* ---- 2 声道：各声道独立，峰值误差 0 ---- */
static void test_meter_stereo(void) {
    enum { FRAMES = 32, CH = 2, BLOCK = 4, HOLD = 8 };
    float src[FRAMES * CH];
    for (int i = 0; i < FRAMES * CH; ++i) src[i] = 0.0f;
    src[1 * CH + 0] = 0.5f;    /* 声道 0 峰值 0.5 */
    src[2 * CH + 1] = 0.75f;   /* 声道 1 峰值 0.75，同一块 */

    float out[FRAMES * CH];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.25f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    CK(mx_meter_process(src, FRAMES, CH, &cfg, out, &rep) == FRAMES);

    float max0 = 0.0f, max1 = 0.0f;
    for (int i = 0; i < FRAMES; ++i) {
        if (out[i * CH + 0] > max0) max0 = out[i * CH + 0];
        if (out[i * CH + 1] > max1) max1 = out[i * CH + 1];
    }
    CK(fabsf(max0 - 0.5f) < 1e-6f);
    CK(fabsf(max1 - 0.75f) < 1e-6f);
    CK(rep.peak_err < 1e-6f);
    CK(rep.hold_observed == HOLD);
    /* 声道 0 的块峰值 0.5 从帧 0 起保持 8 帧 */
    CK(fabsf(out[0 * CH + 0] - 0.5f) < 1e-6f && fabsf(out[7 * CH + 0] - 0.5f) < 1e-6f);
    CK(fabsf(out[8 * CH + 0] - 0.25f) < 1e-6f);
}

/* ---- 6 声道：每声道不同峰值，互不串扰 ---- */
static void test_meter_6ch(void) {
    enum { FRAMES = 48, CH = 6, BLOCK = 8, HOLD = 8 };
    float src[FRAMES * CH];
    for (int i = 0; i < FRAMES * CH; ++i) src[i] = 0.0f;
    float peak[CH] = { 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f };
    for (int c = 0; c < CH; ++c) src[(c + 1) * CH + c] = peak[c];

    float out[FRAMES * CH];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.125f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    CK(mx_meter_process(src, FRAMES, CH, &cfg, out, &rep) == FRAMES);

    for (int c = 0; c < CH; ++c) {
        float m = 0.0f;
        for (int i = 0; i < FRAMES; ++i)
            if (out[i * CH + c] > m) m = out[i * CH + c];
        CK(fabsf(m - peak[c]) < 1e-6f);
        /* 保持期内每声道都是自己的电平 */
        CK(fabsf(out[7 * CH + c] - peak[c]) < 1e-6f);
        /* 回落一步（帧 8 = 块 1 首，块 1 峰值为 0） */
        CK(fabsf(out[8 * CH + c] - (peak[c] - 0.125f)) < 1e-6f);
    }
    CK(rep.peak_err < 1e-6f);
    CK(rep.hold_observed == HOLD);
    CK(rep.overrun_blocks == 0);
}

/* ---- 越界计数与恢复时间 ---- */
static void test_meter_overrun(void) {
    enum { FRAMES = 40, CH = 2, BLOCK = 4, HOLD = 4 };
    float src[FRAMES * CH];
    for (int i = 0; i < FRAMES * CH; ++i) src[i] = 0.0f;
    src[5 * CH + 0] = 1.5f;    /* 块 1（帧 4..7）越界 */
    src[6 * CH + 1] = -1.25f;  /* 块 1 也越界 */
    src[9 * CH + 0] = 1.25f;   /* 块 2（帧 8..11）越界，与块 1 连成一段 */

    float out[FRAMES * CH];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.25f, 1 };
    MxMeterReport rep;
    memset(&rep, 0, sizeof(rep));
    CK(mx_meter_process(src, FRAMES, CH, &cfg, out, &rep) == FRAMES);

    CK(rep.overrun_blocks == 2);
    CK(rep.overrun_frames == 8);
    CK(rep.overrun_runs == 1);
    /* 帧 4..7 保持 1.5；回落到帧 8 得 1.25，恰等于块 2 峰值，表针随之
     * 在 1.25 顶住 4 帧（9..12）；之后 13:1.00 14:0.75 15:0.50
     * 16:0.25 17:0，故最后越界块结束（帧 12）后 5 帧恢复 */
    CK(fabsf(out[4 * CH + 0] - 1.5f) < 1e-6f);
    CK(fabsf(out[7 * CH + 0] - 1.5f) < 1e-6f);
    CK(fabsf(out[8 * CH + 0] - 1.25f) < 1e-6f);
    CK(fabsf(out[12 * CH + 0] - 1.25f) < 1e-6f);
    CK(fabsf(out[13 * CH + 0] - 1.0f) < 1e-6f);
    CK(fabsf(out[17 * CH + 0]) < 1e-6f);
    CK(rep.recovery_frames == 5);
    /* 输出峰值与输入峰值一致（不削顶，只报告越界） */
    CK(fabsf(rep.peak_out - rep.peak_in) < 1e-6f);
}

/* ---- 回落途中出现的较低新峰：抬到该峰值并重新顶住 ---- */
static void test_meter_relower_peak(void) {
    enum { FRAMES = 24, BLOCK = 4, HOLD = 4 };
    float src[FRAMES];
    for (int i = 0; i < FRAMES; ++i) src[i] = 0.0f;
    src[1] = 0.8f;
    src[9] = 0.3f;    /* 块 2（帧 8..11）的峰值，低于原峰但高于当时的衰减值 */

    float out[FRAMES];
    MxMeterCfg cfg = { BLOCK, HOLD, 0.15f, 1 };
    CK(mx_meter_process(src, FRAMES, 1, &cfg, out, NULL) == FRAMES);

    /* 0..3: 0.8；4..7: 0.65/0.50/0.35/0.20；8 抬到 0.3 并重新顶住 4 帧
     * （8..11），12: 0.15，13 起为 0 */
    float expect[FRAMES] = { 0.8f, 0.8f, 0.8f, 0.8f,
                             0.65f, 0.50f, 0.35f, 0.20f,
                             0.30f, 0.30f, 0.30f, 0.30f,
                             0.15f, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < FRAMES; ++i) CK(fabsf(out[i] - expect[i]) < 1e-6f);
}

int main(void) {
    MixBus b;
    CK(mx_init(&b, 8, 2) == 0);
    float a[8], c[8];
    for (int i = 0; i < 8; ++i) { a[i] = 0.25f; c[i] = 0.5f; }
    CK(mx_add(&b, a, 4, 1.0f) == 4);
    CK(mx_add(&b, c, 4, 0.5f) == 4);
    CK(fabsf(b.buf[0] - 0.25f) < 1e-6f);
    CK(fabsf(b.buf[8] - 0.25f) < 1e-6f);
    CK(mx_add(&b, a, 4, 1.0f) == 0);
    mx_free(&b);

    test_meter_peaks();
    test_meter_hand_envelope();
    test_meter_retrigger();
    test_meter_bypass();
    test_meter_errors();
    test_meter_stereo();
    test_meter_6ch();
    test_meter_relower_peak();
    test_meter_overrun();

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
