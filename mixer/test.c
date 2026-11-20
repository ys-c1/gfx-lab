#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void test_duck(void) {
    /* 参数拒绝：电平为零、负增益、释放为负，各自报错码 */
    float bg[4] = {1, 1, 1, 1}, mn[4] = {0, 0, 0, 0};
    MxDuck d = {0.5f, 0.2f, 100.0f, 1};
    CK(mx_duck(bg, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_OK);
    d.level = 0.0f;
    CK(mx_duck(bg, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_ELEVEL);
    d.level = -1.0f;
    CK(mx_duck(bg, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_ELEVEL);
    d.level = 0.5f; d.gain = -0.1f;
    CK(mx_duck(bg, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_EGAIN);
    d.gain = 0.2f; d.release_ms = -5.0f;
    CK(mx_duck(bg, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_ERELEASE);
    d.release_ms = 100.0f;
    CK(mx_duck(NULL, mn, 2, 2, 48000.0f, &d, NULL) == MX_DUCK_EARG);

    /* 旁通：enabled=0 与 cfg=NULL 都必须逐样本相同 */
    float b2[8], orig[8];
    for (int i = 0; i < 8; ++i) b2[i] = orig[i] = 0.1f * (float)i - 0.3f;
    MxDuck off = {0.5f, 0.2f, 100.0f, 0};
    CK(mx_duck(b2, mn, 4, 2, 48000.0f, &off, NULL) == MX_DUCK_OK);
    CK(memcmp(b2, orig, sizeof b2) == 0);
    CK(mx_duck(b2, mn, 4, 2, 48000.0f, NULL, NULL) == MX_DUCK_OK);
    CK(memcmp(b2, orig, sizeof b2) == 0);

    /* 与手算包络对照：单声道，rate=1000Hz，release=50ms -> coef=exp(-0.02) */
    enum { N = 400 };
    float main1[N], bg1[N], expect[N];
    for (int i = 0; i < N; ++i) {
        main1[i] = i < 100 ? 0.8f : 0.0f;
        bg1[i] = 0.5f;
    }
    MxDuck cfg = {0.5f, 0.25f, 50.0f, 1};
    float depth_db = -1.0f;
    CK(mx_duck(bg1, main1, N, 1, 1000.0f, &cfg, &depth_db) == MX_DUCK_OK);
    float coef = expf(-1.0f / (0.050f * 1000.0f));
    float env = 0.0f, maxdiff = 0.0f;
    for (int i = 0; i < N; ++i) {
        float m = fabsf(main1[i]);
        env = m > env * coef ? m : env * coef;
        float x = env / 0.5f; if (x > 1.0f) x = 1.0f;
        expect[i] = 0.5f * (1.0f - (1.0f - 0.25f) * x);
        float diff = fabsf(bg1[i] - expect[i]);
        if (diff > maxdiff) maxdiff = diff;
    }
    CK(maxdiff < 1e-6f);
    /* 压缩深度回报：主轨 0.8 > level 0.5，满深度 = -20log10(0.25) ≈ 12.0412 dB */
    float expect_db = -20.0f * log10f(0.25f);
    CK(fabsf(depth_db - expect_db) < 1e-3f);
    /* 主轨不达电平时深度按比例缩小：主轨 0.25 -> x 峰值 0.5 -> g 最小 0.625 */
    for (int i = 0; i < N; ++i) { main1[i] = i < 100 ? 0.25f : 0.0f; bg1[i] = 0.5f; }
    CK(mx_duck(bg1, main1, N, 1, 1000.0f, &cfg, &depth_db) == MX_DUCK_OK);
    float expect_db2 = -20.0f * log10f(1.0f - 0.75f * 0.5f);
    CK(fabsf(depth_db - expect_db2) < 1e-3f);
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
    test_duck();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
