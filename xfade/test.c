#include "xfade.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int approx(float x, float y, float eps) { return fabsf(x - y) < eps; }

int main(void) {
    /* ---- 参数校验：每条错误都要能指出字段 ---- */
    XfConfig bad;
    bad = (XfConfig){0, 1, XF_LINEAR, 100};
    CK(xf_validate(&bad) == XF_E_RATE);
    CK(strcmp(xf_error_field(XF_E_RATE), "sample_rate") == 0);

    bad = (XfConfig){48000, 0, XF_LINEAR, 100};
    CK(xf_validate(&bad) == XF_E_CHANNELS);
    CK(strcmp(xf_error_field(XF_E_CHANNELS), "channels") == 0);

    bad = (XfConfig){48000, 1, (XfCurve)99, 100};
    CK(xf_validate(&bad) == XF_E_CURVE);

    bad = (XfConfig){48000, 1, XF_LINEAR, 0};      /* 时长为零 */
    CK(xf_validate(&bad) == XF_E_DURATION);
    CK(strcmp(xf_error_field(XF_E_DURATION), "fade_ms/curve") == 0);

    bad = (XfConfig){48000, 1, XF_POWER, -5};      /* 时长非法 */
    CK(xf_validate(&bad) == XF_E_DURATION);

    bad = (XfConfig){48000, 1, XF_NONE, 10};       /* 不淡化却带时长 */
    CK(xf_validate(&bad) == XF_E_CURVE);

    CK(xf_validate(NULL) == XF_E_NULL);
    CK(strcmp(xf_error_field(XF_E_OVERLAP), "fade_ms(overlap)") == 0);

    /* ---- 重叠帧数：换算与采样率无关的等价性 ---- */
    XfConfig c48 = {48000, 2, XF_LINEAR, 100};
    XfConfig c96 = {96000, 2, XF_LINEAR, 100};
    CK(xf_overlap_frames(&c48) == 4800);
    CK(xf_overlap_frames(&c96) == 9600);
    XfConfig none = {48000, 2, XF_NONE, 0};
    CK(xf_overlap_frames(&none) == 0);
    CK(xf_overlap_frames(&c48) == (int)(48000 * 0.1 + 0.5));

    /* ---- 第 1 段：曲线性质 ---- */
    float wa, wb;
    CK(xf_curve_point(XF_LINEAR, 0.0f, &wa, &wb) == XF_OK && approx(wa, 1, 1e-7) && approx(wb, 0, 1e-7));
    CK(xf_curve_point(XF_LINEAR, 1.0f, &wa, &wb) == XF_OK && approx(wa, 0, 1e-7) && approx(wb, 1, 1e-7));
    CK(xf_curve_point(XF_LINEAR, 0.3f, &wa, &wb) == XF_OK && approx(wa + wb, 1.0f, 1e-7));
    /* t 越界自动夹紧 */
    CK(xf_curve_point(XF_LINEAR, -2.0f, &wa, &wb) == XF_OK && approx(wa, 1, 1e-7));
    CK(xf_curve_point(XF_LINEAR, 9.0f, &wa, &wb) == XF_OK && approx(wb, 1, 1e-7));

    CK(xf_curve_point(XF_POWER, 0.0f, &wa, &wb) == XF_OK && approx(wa, 1, 1e-7) && approx(wb, 0, 1e-7));
    CK(xf_curve_point(XF_POWER, 1.0f, &wa, &wb) == XF_OK && approx(wa, 0, 1e-7) && approx(wb, 1, 1e-7));
    /* 等功率：平方和恒为 1；中点两权重相等 */
    for (int k = 0; k <= 10; ++k) {
        float t = k / 10.0f;
        xf_curve_point(XF_POWER, t, &wa, &wb);
        CK(approx(wa * wa + wb * wb, 1.0f, 1e-6));
    }
    xf_curve_point(XF_POWER, 0.5f, &wa, &wb);
    CK(approx(wa, wb, 1e-7) && approx(wa, 0.70710678f, 1e-6));
    CK(xf_curve_point((XfCurve)7, 0.5f, &wa, &wb) == XF_E_CURVE);

    /* 权重表：端点与长度 */
    float *twa = NULL, *twb = NULL;
    CK(xf_build_weights(XF_POWER, 101, &twa, &twb) == XF_OK);
    CK(approx(twa[0], 1, 1e-7) && approx(twb[0], 0, 1e-7));
    CK(approx(twa[100], 0, 1e-7) && approx(twb[100], 1, 1e-7));
    CK(approx(twa[50], twb[50], 1e-7));
    free(twa); free(twb);
    CK(xf_build_weights(XF_LINEAR, 0, &twa, &twb) == XF_OK && twa == NULL && twb == NULL);
    CK(xf_build_weights((XfCurve)3, 4, &twa, &twb) == XF_E_CURVE);
    CK(xf_build_weights(XF_LINEAR, -1, &twa, &twb) == XF_E_DURATION);

    /* ---- 第 2 段：不做淡化时输出照旧（逐样本等于硬拼接） ---- */
    enum { N = 8, CH = 2 };
    float A[N * CH], B[N * CH], out[2 * N * CH], ref[2 * N * CH];
    for (int i = 0; i < N * CH; ++i) { A[i] = 0.10f + 0.01f * i; B[i] = -0.20f - 0.02f * i; }
    memcpy(ref, A, sizeof(A));
    memcpy(ref + N * CH, B, sizeof(B));

    int got_n = -1;
    CK(xf_run(&none, A, N, B, N, out, 2 * N, &got_n) == XF_OK);
    CK(got_n == 2 * N);
    CK(memcmp(out, ref, sizeof(ref)) == 0);
    /* 容量不足报错并指出字段 */
    CK(xf_run(&none, A, N, B, N, out, 2 * N - 1, &got_n) == XF_E_CAP);
    CK(xf_run(&none, NULL, N, B, N, out, 2 * N, &got_n) == XF_E_NULL);

    /* ---- 线性交叉合成：长度、纯区、交叉区逐样本值（直接用权重表驱动） ---- */
    float wa4[4] = {1.0f, 2.0f / 3.0f, 1.0f / 3.0f, 0.0f};
    float wb4[4] = {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
    int total = xf_cross(A, N, B, N, CH, wa4, wb4, 4, out, 2 * N - 4);
    CK(total == 2 * N - 4);
    CK(memcmp(out, A, (size_t)(N - 4) * CH * sizeof(float)) == 0);          /* 头部纯 A */
    CK(approx(out[((N - 4) + 1) * CH + 0],
              A[(N - 4 + 1) * CH] * wa4[1] + B[1 * CH] * wb4[1], 1e-6));     /* 交叉点 */
    CK(approx(out[(total - 1) * CH + 1], B[(N - 1) * CH + 1], 1e-6));        /* 尾部纯 B */
    CK(xf_cross(A, N, B, N, CH, wa4, wb4, N + 1, out, 99) == XF_E_OVERLAP);  /* 重叠超长 */

    /* 端到端 xf_run：线性 50ms @48k = 2400 帧，常量信号幅度守恒 */
    XfConfig lin50 = {48000, 1, XF_LINEAR, 50};
    CK(xf_overlap_frames(&lin50) == 2400);
    enum { M = 4800 };
    float *ca = malloc(M * sizeof(float)), *cb = malloc(M * sizeof(float));
    float *co = malloc((2 * M) * sizeof(float));
    for (int i = 0; i < M; ++i) { ca[i] = 0.6f; cb[i] = 0.6f; }
    int on = 0;
    CK(xf_run(&lin50, ca, M, cb, M, co, 2 * M, &on) == XF_OK);
    CK(on == 2 * M - 2400);
    int bad_sample = 0;
    for (int i = 0; i < on; ++i) if (!approx(co[i], 0.6f, 1e-6)) bad_sample = 1;
    CK(!bad_sample);
    /* 重叠超过素材长度：报错并指出字段 */
    XfConfig huge = {48000, 1, XF_LINEAR, 5000};
    CK(xf_run(&huge, ca, M, cb, M, co, 2 * M, &on) == XF_E_OVERLAP);

    /* 采样率无关性：权重只由归一化 t=i/(n-1) 决定，与帧数/采样率无关。
     * 48k(4800 帧) 与 96k(9600 帧) 两张表，逐点都应等于同一条连续归一化曲线；
     * 两表端点严格一致（同一 t=0/1）。 */
    float *w48 = NULL, *w48b = NULL, *w96 = NULL, *w96b = NULL;
    CK(xf_build_weights(XF_POWER, 4800, &w48, &w48b) == XF_OK);
    CK(xf_build_weights(XF_POWER, 9600, &w96, &w96b) == XF_OK);
    int mismatch = 0;
    for (int i = 0; i < 4800; ++i) {
        float t = (float)i / 4799.0f, ca, cb;
        xf_curve_point(XF_POWER, t, &ca, &cb);
        if (!approx(w48[i], ca, 1e-6) || !approx(w48b[i], cb, 1e-6)) mismatch = 1;
    }
    for (int i = 0; i < 9600; ++i) {
        float t = (float)i / 9599.0f, ca, cb;
        xf_curve_point(XF_POWER, t, &ca, &cb);
        if (!approx(w96[i], ca, 1e-6) || !approx(w96b[i], cb, 1e-6)) mismatch = 1;
    }
    CK(!mismatch);
    CK(approx(w48[0], w96[0], 1e-7) && approx(w48b[4799], w96b[9599], 1e-7));
    free(w48); free(w48b); free(w96); free(w96b);

    /* ---- 第 3 段：接缝跳变检查 ---- */
    /* 跳变在样本 3->4 之间，seam 给“跳变对的左帧”下标 3 */
    float sig[10] = {0, 0, 0, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    float jmp = -1; int at = -1;
    CK(xf_seam_jump(sig, 10, 1, 3, 0, &jmp, &at) == XF_OK);
    CK(approx(jmp, 0.5f, 1e-7) && at == 3);
    CK(xf_seam_jump(sig, 10, 1, 3, 2, &jmp, &at) == XF_OK && approx(jmp, 0.5f, 1e-7));
    CK(xf_seam_jump(sig, 10, 1, 0, 0, &jmp, &at) == XF_OK && approx(jmp, 0.0f, 1e-7));
    CK(xf_seam_jump(sig, 10, 1, 8, 0, &jmp, &at) == XF_OK);   /* 最后一个合法接缝对 */
    CK(xf_seam_jump(sig, 10, 1, 9, 0, &jmp, &at) == XF_E_NULL); /* 末帧无右侧样本 */
    CK(xf_seam_jump(sig, 10, 1, 10, 0, &jmp, &at) == XF_E_NULL);
    /* 立体声：取每声道最大差；跳变对为帧 1->2，seam=1 */
    float st[8] = {0, 0, 0, 0, 0.2f, 0.9f, 0.2f, 0.9f};
    CK(xf_seam_jump(st, 4, 2, 1, 0, &jmp, &at) == XF_OK && approx(jmp, 0.9f, 1e-7));

    /* ---- 权重曲线落盘并回读校验 ---- */
    const char *csv = "build/curve_test.csv";
    CK(xf_save_curve(csv, XF_POWER, 257) == XF_OK);
    CK(xf_export_curve(csv, &lin50, 257) == XF_OK);
    FILE *f = fopen(csv, "r");
    CK(f != NULL);
    if (f) {
        char line[128];
        CK(fgets(line, sizeof line, f) != NULL);       /* 表头 */
        CK(strncmp(line, "i,t,wa,wb", 9) == 0);
        int rows = 0; float first_wa = -1, last_wb = -1;
        while (fgets(line, sizeof line, f)) {
            int i; float t, a, b; char curve[32];
            if (sscanf(line, "%d,%f,%f,%f,%31s", &i, &t, &a, &b, curve) == 5) {
                if (rows == 0) first_wa = a;
                last_wb = b;
                ++rows;
            }
        }
        CK(rows == 257);
        CK(approx(first_wa, 1.0f, 1e-6) && approx(last_wb, 1.0f, 1e-6));
        fclose(f);
    }
    CK(xf_save_curve("build/no/such/dir/c.csv", XF_LINEAR, 10) == XF_E_IO);
    CK(xf_save_curve(csv, (XfCurve)4, 10) == XF_E_CURVE);
    CK(xf_save_curve(csv, XF_LINEAR, 0) == XF_E_DURATION);

    free(ca); free(cb); free(co);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
