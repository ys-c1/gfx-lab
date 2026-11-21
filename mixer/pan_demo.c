/* 声像演示：同一段素材分别用线性与等功率声像扫过左中右，落盘 WAV。
 * 关闭（enabled=0）时输出与 mx_add 原样结果逐字节一致，本程序也会校验并打印。
 * 不触碰 build/demo 与 demo.wav —— 旧演示产物保持逐字节不变。 */
#include "mixer.h"
#include "pan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000
#define FRAMES 4800 /* 0.1 s */
#define CH 2

#ifndef MX_PAN_PI
#define MX_PAN_PI 3.14159265358979323846
#endif

static int write_wav(const char *path, const float *buf, int frames, int ch) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned int data_bytes = (unsigned int)(frames * ch * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, c = (unsigned short)ch;
    unsigned int rate = SR, byte_rate = rate * c * 2;
    unsigned short align = (unsigned short)(c * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&c, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (long i = 0; i < (long)frames * ch; ++i) {
        float v = buf[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        short s = (short)lrintf(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

/* 单声道源铺到两条声道：约 0.36 峰值，移动时不会因增益越界削波。 */
static void make_source(float *interleaved) {
    for (int i = 0; i < FRAMES; ++i) {
        float v = 0.36f * sinf(2.0f * (float)MX_PAN_PI * 440.0f * (float)i / (float)SR);
        interleaved[2 * i] = v;
        interleaved[2 * i + 1] = v;
    }
}

/* 扫过 [-1,+1] 的等功率声像：按帧分 8 段，每段固定位置。 */
static void fill_autopan(float *out) {
    const int seg = FRAMES / 8;
    for (int i = 0; i < FRAMES; ++i) {
        int k = i / seg;
        float p = -1.0f + 2.0f * (float)k / 7.0f;
        float gl, gr;
        mx_pan_gains(MX_PAN_EQUAL_POWER, p, &gl, &gr);
        float v = 0.36f * sinf(2.0f * (float)MX_PAN_PI * 440.0f * (float)i / (float)SR);
        out[2 * i] = v * gl;
        out[2 * i + 1] = v * gr;
    }
}

static void run_case(const char *tag, MxPanLaw law, float pos) {
    MixBus b;
    if (mx_init(&b, FRAMES, CH) != 0) return;
    float *src = (float*)malloc(sizeof(float) * FRAMES * CH);
    make_source(src);
    mx_add(&b, src, FRAMES, 1.0f);

    MxPanCfg cfg = { 1, (int)law, pos };
    MxPanStats st;
    MxPanErr err;
    int rc = mx_pan_apply(&b, &cfg, &st, &err);
    if (rc != MX_PAN_OK) {
        printf("%s: rejected: %s\n", tag, err.msg);
    } else {
        printf("%-12s law=%-11s pos=%+.2f  gL=%.6f gR=%.6f  energy=%.6f  "
               "pos_est=%+.6f  pairs=%d  samples=%lld\n",
               tag, mx_pan_law_name(law), pos, st.gL, st.gR, st.energy,
               st.pos_est, st.pairs, st.samples);
        char path[64];
        snprintf(path, sizeof(path), "demo_pan_%s.wav", tag);
        write_wav(path, b.buf, b.frames, b.channels);
    }
    mx_free(&b);
    free(src);
}

/* 关闭路径：与直接 mx_add 的缓冲逐字节比较（即“和现在一模一样”）。 */
static void check_bypass(void) {
    MixBus ref, got;
    mx_init(&ref, FRAMES, CH);
    mx_init(&got, FRAMES, CH);
    float *src = (float*)malloc(sizeof(float) * FRAMES * CH);
    make_source(src);
    mx_add(&ref, src, FRAMES, 1.0f);
    mx_add(&got, src, FRAMES, 1.0f);
    MxPanCfg off = { 0, MX_PAN_EQUAL_POWER, 0.7f };
    mx_pan_apply(&got, &off, NULL, NULL);
    int same = memcmp(ref.buf, got.buf, sizeof(float) * FRAMES * CH) == 0;
    printf("bypass (enabled=0): %s (%zu bytes compared)\n",
           same ? "byte-identical" : "DIFFERENT", sizeof(float) * FRAMES * CH);
    free(src);
    mx_free(&ref);
    mx_free(&got);
}

int main(void) {
    check_bypass();
    run_case("linear_c", MX_PAN_LINEAR, 0.0f);
    run_case("ep_c", MX_PAN_EQUAL_POWER, 0.0f);
    run_case("linear_l", MX_PAN_LINEAR, -0.5f);
    run_case("ep_l", MX_PAN_EQUAL_POWER, -0.5f);
    run_case("linear_r", MX_PAN_LINEAR, 0.5f);
    run_case("ep_r", MX_PAN_EQUAL_POWER, 0.5f);

    MixBus sw;
    if (mx_init(&sw, FRAMES, CH) == 0) {
        float *out = (float*)malloc(sizeof(float) * FRAMES * CH);
        fill_autopan(out);
        printf("autopan: demo_pan_sweep.wav written\n");
        write_wav("demo_pan_sweep.wav", out, FRAMES, CH);
        free(out);
        mx_free(&sw);
    }

    /* 非法组合示例：一次列出全部冲突项 */
    MixBus b;
    if (mx_init(&b, FRAMES, 2) == 0) {
        mx_add(&b, (float[4]){0.1f, 0.1f, 0.1f, 0.1f}, 2, 1.0f);
        MxPanCfg bad = { 2, (MxPanLaw)9, 3.0f };
        MxPanErr err;
        mx_pan_apply(&b, &bad, NULL, &err);
        printf("rejected example (%d conflicts): %s\n", err.count, err.msg);
        mx_free(&b);
    }
    return 0;
}
