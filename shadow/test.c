#include "shadow.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- legacy grayscale behavior (must stay unchanged) ---- */
static void test_legacy(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);
    sh_free(&m);
}

/* ---- tint switch off: new API must reproduce the gray path ---- */
static void test_switch_off_unchanged(void) {
    ShFrame f;
    CK(sh_frame_init(&f, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    ShTune off = {0};
    CK(sh_render_tinted(&f, &l, &s, 1, &off, 0) == SH_OK);

    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    CK(sh_render(&m, &l, &s, 1) == 0);
    int mism = 0;
    for (int i = 0; i < 64 * 64; ++i) {
        float v = m.buf[i];
        if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
        unsigned char g = (unsigned char)(v * 255.0f + 0.5f);
        if (f.rgb[i * 3] != g || f.rgb[i * 3 + 1] != g || f.rgb[i * 3 + 2] != g)
            ++mism;
    }
    CK(mism == 0);
    sh_free(&m);
    sh_frame_free(&f);
}

/* ---- two-stage math: base tint from light color, then mix by occlusion ---- */
static void test_two_stage_math(void) {
    ShFrame f;
    CK(sh_frame_init(&f, 64, 64) == 0);
    sh_frame_fill(&f, 200, 200, 200);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};

    ShTune t = {0};
    t.enable = 1;
    t.color_source = SH_SRC_RGB;
    t.color = (ShColor){255, 100, 20};
    t.shadow_strength = 0.6f;
    t.penumbra = 0.0f;
    CK(sh_render_tinted(&f, &l, &s, 1, &t, 0) == SH_OK);

    /* fully occluded pixel (hard shadow, o=1): result must equal the
     * stage-1 base = light * (1 - strength), independent of backdrop */
    unsigned char br = (unsigned char)(255 * (1.0f - 0.6f) + 0.5f);
    unsigned char bg_ = (unsigned char)(100 * (1.0f - 0.6f) + 0.5f);
    unsigned char bb = (unsigned char)(20 * (1.0f - 0.6f) + 0.5f);
    size_t idx = ((size_t)40 * 64 + 32) * 3;
    CK(f.rgb[idx + 0] == br);
    CK(f.rgb[idx + 1] == bg_);
    CK(f.rgb[idx + 2] == bb);
    /* shadow is tinted, not gray: channels differ */
    CK(f.rgb[idx + 0] != f.rgb[idx + 2]);

    /* lit pixel stays at the backdrop (o = 0, no mixing) */
    size_t lit = ((size_t)2 * 64 + 32) * 3;
    CK(f.rgb[lit + 0] == 200 && f.rgb[lit + 1] == 200 && f.rgb[lit + 2] == 200);
    sh_frame_free(&f);
}

/* ---- soft penumbra gives fractional occlusion ---- */
static void test_penumbra(void) {
    ShFrame f;
    CK(sh_frame_init(&f, 64, 64) == 0);
    sh_frame_fill(&f, 200, 200, 200);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    ShTune t = {0};
    t.enable = 1; t.color_source = SH_SRC_RGB;
    t.color = (ShColor){255, 100, 20};
    t.shadow_strength = 0.6f; t.penumbra = 4.0f;
    CK(sh_render_tinted(&f, &l, &s, 1, &t, 0) == SH_OK);

    ShadowMap mm = {64, 64, NULL};
    float occ[64 * 64];
    CK(sh_occlusion(&mm, &l, &s, 1, 4.0f, occ) == SH_OK);
    int fractional = 0;
    for (int i = 0; i < 64 * 64; ++i)
        if (occ[i] > 0.0f && occ[i] < 1.0f) ++fractional;
    CK(fractional > 0);
    sh_frame_free(&f);
}

/* ---- conflicting color configurations must error and name both sides ---- */
static void test_conflicts(void) {
    char a[32], b[32];
    ShTune t = {0};
    t.enable = 1;
    t.color_source = SH_SRC_RGB;
    t.color = (ShColor){255, 0, 0};
    t.preset = SH_PRESET_COOL;   /* preset also configured -> conflict */
    t.swatch_id = -1;
    int rc = sh_tune_check(&t, a, sizeof a, b, sizeof b);
    CK(rc == SH_ECONFLICT);
    CK(strcmp(a, "color.rgb") == 0);
    CK(strcmp(b, "preset") == 0);
    CK(strstr(sh_strerror(rc), "conflict") != NULL);

    /* preset vs swatch (both fields configured; selector points at swatch) */
    ShTune t2 = {0};
    t2.enable = 1; t2.color_source = SH_SRC_SWATCH; t2.preset = SH_PRESET_DUSK;
    t2.swatch_id = 2;
    rc = sh_tune_check(&t2, a, sizeof a, b, sizeof b);
    CK(rc == SH_ECONFLICT);
    CK(strcmp(a, "preset") == 0 && strcmp(b, "swatch") == 0);

    /* single source is fine */
    ShTune ok = {0};
    ok.enable = 1; ok.color_source = SH_SRC_RGB;
    ok.color = (ShColor){10, 20, 30};
    ok.preset = 0; ok.swatch_id = -1;
    CK(sh_tune_check(&ok, a, sizeof a, b, sizeof b) == SH_OK);

    /* out-of-range strength rejected */
    ShTune bad = ok; bad.shadow_strength = 1.5f;
    CK(sh_tune_check(&bad, NULL, 0, NULL, 0) == SH_ERANGE);

    /* unknown swatch id */
    ShTune nosw = {0};
    nosw.enable = 1; nosw.color_source = SH_SRC_SWATCH;
    nosw.swatch_id = 999;
    CK(sh_tune_check(&nosw, NULL, 0, NULL, 0) == SH_ENOSWATCH);
}

/* ---- swatch cards are retrievable ---- */
static void test_swatches(void) {
    int n = sh_swatch_count();
    CK(n >= 4);
    ShColor c;
    char name[32];
    CK(sh_swatch_get(0, &c, name, sizeof name) == SH_OK);
    CK(name[0] != '\0');
    CK(sh_swatch_get(n, &c, name, sizeof name) == SH_ENOSWATCH);

    /* a tune sourced from a swatch resolves to the swatch color */
    ShTune t = {0};
    t.enable = 1; t.color_source = SH_SRC_SWATCH; t.swatch_id = 2;
    ShColor got;
    CK(sh_resolve_color(&t, &got) == SH_OK);
    ShColor expect;
    CK(sh_swatch_get(2, &expect, NULL, 0) == SH_OK);
    CK(got.r == expect.r && got.g == expect.g && got.b == expect.b);
}

/* ---- slow vs fast gear: identical output, exhaustive cross-check ---- */
static void test_gears_match(void) {
    const int W = 96, H = 96, N = 40;
    ShLight l = {48.0f, 12.0f, 30.0f, 1.0f};

    /* build a deterministic scene through the bench RNG by running both
     * gears on identical bench frames instead: reuse sh_bench_run */
    ShTune t = {0};
    t.enable = 1; t.color_source = SH_SRC_RGB;
    t.color = (ShColor){255, 140, 40};
    t.shadow_strength = 0.55f; t.penumbra = 3.0f;

    ShFrame fs, ff;
    CK(sh_frame_init(&fs, W, H) == 0);
    CK(sh_frame_init(&ff, W, H) == 0);
    ShBench bs, bf;
    CK(sh_bench_run(&fs, &l, N, &t, 0, &bs) == SH_OK);
    CK(sh_bench_run(&ff, &l, N, &t, 1, &bf) == SH_OK);
    CK(bs.pixels == W * H && bf.pixels == W * H);
    CK(bs.shadow_pixels == bf.shadow_pixels);

    int diff = 0;
    for (int i = 0; i < W * H * 3; ++i)
        if (fs.rgb[i] != ff.rgb[i]) ++diff;
    CK(diff == 0);

    /* exhaustive reference cross-check on both gears.
     * sh_test_scene regenerates the exact deterministic bench scene. */
    ShSeg segs[64];
    sh_test_scene(W, H, N, segs);
    unsigned char bg[3] = {210, 210, 210};
    long mm_s = -1, mm_f = -1;
    double md_s = -1.0, md_f = -1.0;
    CK(sh_bench_verify_exhaustive(&fs, &l, segs, N, &t, bg, &mm_s, &md_s) == SH_OK);
    CK(sh_bench_verify_exhaustive(&ff, &l, segs, N, &t, bg, &mm_f, &md_f) == SH_OK);
    CK(mm_s == 0);
    CK(mm_f == 0);
    CK(md_s <= 0.5 && md_f <= 0.5);
    sh_frame_free(&fs);
    sh_frame_free(&ff);
}

int main(void) {
    test_legacy();
    test_switch_off_unchanged();
    test_two_stage_math();
    test_penumbra();
    test_conflicts();
    test_swatches();
    test_gears_match();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
