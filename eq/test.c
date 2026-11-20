#include "eq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static EqConfig base_cfg(void) {
    EqConfig c = { 48000, 2, 1, 250.0f, 4000.0f, 1.0f, 1.0f, 1.0f };
    return c;
}

int main(void) {
    EqConfig cc;
    /* 1. 参数校验：错误码 + 字段名 */
    EqConfig c = base_cfg();
    CK(eq_validate(&c) == EQ_OK);
    EqConfig bad;
    bad = c; bad.sample_rate = 0;
    CK(eq_validate(&bad) == EQ_ERR_RATE);
    bad = c; bad.channels = 9;
    CK(eq_validate(&bad) == EQ_ERR_CHANNELS);
    bad = c; bad.enabled = 7;
    CK(eq_validate(&bad) == EQ_ERR_ENABLE);
    bad = c; bad.f_lo = 5000.0f; bad.f_hi = 4000.0f; /* 边界倒置 */
    CK(eq_validate(&bad) == EQ_ERR_FLO);
    bad = c; bad.f_hi = 24001.0f; /* 超过 Nyquist */
    CK(eq_validate(&bad) == EQ_ERR_FHI);
    bad = c; bad.g_lo = 17.0f;
    CK(eq_validate(&bad) == EQ_ERR_GAIN_LO);
    bad = c; bad.g_mid = -0.1f;
    CK(eq_validate(&bad) == EQ_ERR_GAIN_MID);
    bad = c; bad.g_hi = NAN;
    CK(eq_validate(&bad) == EQ_ERR_GAIN_HI);
    printf("field for EQ_ERR_FLO: %s\n", eq_strerror(EQ_ERR_FLO));
    printf("field for EQ_ERR_GAIN_HI: %s\n", eq_strerror(EQ_ERR_GAIN_HI));
    EqState should_not_touch; memset(&should_not_touch, 0x55, sizeof should_not_touch);
    CK(eq_init(&should_not_touch, &bad) == EQ_ERR_GAIN_HI);
    CK(should_not_touch.ready == 0);

    /* 2. 关闭均衡：逐样本完全一致（位一致，含原地处理） */
    EqConfig off = base_cfg(); off.enabled = 0;
    EqState s; CK(eq_init(&s, &off) == EQ_OK);
    enum { N = 2048 };
    float in[N * 2], out[N * 2];
    srand(1234);
    for (int i = 0; i < N * 2; ++i) {
        in[i] = ((float)rand() / (float)RAND_MAX - 0.5f) * 1.8f;
    }
    CK(eq_process(&s, in, out, N) == N);
    CK(memcmp(in, out, sizeof in) == 0);
    float in2[N * 2]; memcpy(in2, in, sizeof in);
    CK(eq_process(&s, in2, in2, N) == N);
    CK(memcmp(in, in2, sizeof in) == 0);
    eq_free(&s);

    /* 3. 单位增益启用：完全重构（互补分频）+ 零输入稳定 */
    EqState u; cc = base_cfg(); CK(eq_init(&u, &cc) == EQ_OK);
    CK(eq_process(&u, in, out, N) == N);
    float maxerr = 0.0f;
    for (int i = 0; i < N * 2; ++i) {
        float e = fabsf(out[i] - in[i]);
        if (e > maxerr) maxerr = e;
    }
    CK(maxerr < 1e-5f);
    float zeros[64] = {0}, zout[64];
    CK(eq_process(&u, zeros, zout, 32) == 32);
    for (int i = 0; i < 64; ++i) CK(zout[i] == 0.0f);
    eq_free(&u);

    /* 4. 幅频响应：截止点 -3dB；通带/阻带方向正确；功率互补 |LP|^2+|HP|^2=1 */
    EqState r; cc = base_cfg(); CK(eq_init(&r, &cc) == EQ_OK);
    const EqBank *bk = eq_bank(&r);
    float mlo = eq_mag(&bk->lp_lo, 250.0f, 48000);
    CK(fabsf(mlo - 0.707107f) < 2e-4f); /* 1/sqrt(2)，即 -3.01 dB */
    CK(eq_mag(&bk->lp_lo, 20.0f, 48000) > 0.99f);
    CK(eq_mag(&bk->hp_lo, 20000.0f, 48000) > 0.99f);
    /* 高通在 DC 趋零（1Hz 处理论约 f/f_c≈0.004）、低通在 Nyquist 趋零 */
    CK(eq_mag(&bk->hp_lo, 1.0f, 48000) < 5e-3f);
    CK(eq_mag(&bk->lp_lo, 23999.0f, 48000) < 1e-3f);
    for (int fi = 0; fi < 5; ++fi) {
        float f = 30.0f * (float)(1 << (fi * 2)); /* 30..480 Hz 多点 */
        float ml = eq_mag(&bk->lp_lo, f, 48000);
        float mh = eq_mag(&bk->hp_lo, f, 48000);
        CK(fabsf(ml * ml + mh * mh - 1.0f) < 1e-6f);
    }
    eq_free(&r);

    /* 5. 正弦扫描：80Hz 只被低段增益放大，中/高增益对它几乎无影响 */
    EqConfig gcfg = base_cfg();
    gcfg.g_lo = 2.0f; gcfg.g_mid = 0.5f; gcfg.g_hi = 4.0f;
    EqState g; CK(eq_init(&g, &gcfg) == EQ_OK);
    float tone[N * 2];
    for (int i = 0; i < N; ++i) {
        float v = 0.4f * sinf(2.0f * (float)M_PI * 80.0f * (float)i / 48000.0f);
        tone[i * 2] = v; tone[i * 2 + 1] = v;
    }
    float tout[N * 2]; CK(eq_process(&g, tone, tout, N) == N);
    float amp = 0.0f;
    for (int i = N / 2; i < N; ++i) if (fabsf(tout[i * 2]) > amp) amp = fabsf(tout[i * 2]);
    /* 80Hz 处复域总响应（低段主导，中/高支路相位不同需复域求和），由 eq_response 给出 */
    float re5, im5;
    CK(eq_response(&g, 80.0f, &re5, &im5) == EQ_OK);
    float expect_amp = 0.4f * hypotf(re5, im5);
    CK(fabsf(amp - expect_amp) < 0.02f);
    /* 单位增益时复域总响应恒为 1（完全重构的频域体现） */
    EqState ug; cc = base_cfg(); CK(eq_init(&ug, &cc) == EQ_OK);
    for (int fi = 0; fi < 6; ++fi) {
        float f = 50.0f * (float)(1 << fi);
        float rr, ii;
        CK(eq_response(&ug, f, &rr, &ii) == EQ_OK);
        CK(fabsf(hypotf(rr, ii) - 1.0f) < 1e-6f);
    }
    CK(eq_response(&ug, 30000.0f, &re5, &im5) == EQ_ERR_FHI);
    eq_free(&g);
    eq_free(&ug);

    /* 6. 峰值与过载计数 */
    EqState o; cc = base_cfg(); CK(eq_init(&o, &cc) == EQ_OK);
    float hot[6] = { 0.9f, -1.2f, 0.3f, -0.2f, 1.5f, 0.1f }; /* 2 声道 */
    float hout[6];
    CK(eq_process(&o, hot, hout, 3) == 3);
    float pi[2], po[2]; long ov[2];
    CK(eq_stats_get(&o, pi, po, ov) == 2);
    CK(fabsf(pi[0] - 1.5f) < 1e-6f && fabsf(pi[1] - 1.2f) < 1e-6f);
    /* 单位增益下输出峰值等于输入（互补重构），过载按输出判定 */
    CK(ov[0] == 1 && ov[1] == 1);
    CK(eq_reset(&o) == EQ_OK);
    CK(eq_stats_get(&o, pi, po, ov) == 2);
    CK(pi[0] == 0.0f && ov[1] == 0);
    eq_free(&o);

    /* 7. 系数导出文件可读且字段完整 */
    EqState e; cc = base_cfg(); CK(eq_init(&e, &cc) == EQ_OK);
    CK(eq_export_coef(&e, "build/coef.csv") == EQ_OK);
    FILE *f = fopen("build/coef.csv", "r");
    CK(f != NULL);
    if (f) {
        char line[256]; int sections = 0;
        while (fgets(line, sizeof line, f))
            if (line[0] != '#') sections++;
        CK(sections == 4);
        fclose(f);
    }
    eq_free(&e);

    /* 8. 运行期参数检查 */
    CK(eq_process(NULL, in, out, N) == EQ_ERR_NULL);
    EqState z; cc = base_cfg(); CK(eq_init(&z, &cc) == EQ_OK);
    CK(eq_process(&z, in, out, -1) == EQ_ERR_FRAMES);
    CK(eq_process(&z, NULL, out, 1) == EQ_ERR_NULL);
    eq_free(&z);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
