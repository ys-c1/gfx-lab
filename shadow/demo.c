#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

/* Benchmark: two-tier cascade (near 1024 / far 256) against single-tier
 * controls at uniform 256 and uniform 1024, plus a direct per-pixel ground
 * truth. Metrics per run: clarity RMSE vs ground truth (overall and split by
 * the near/far boundary), ray/segment sampling count, and wall time.
 *
 * The output covers world box [0,512]^2 so its pixels line up 1:1 with the
 * sh_render ground truth. Camera at (256,256); farthest corner is at
 * distance 362.03, so far=384 leaves every pixel covered. */

#define OUT 512                 /* output map size, 1.0 world unit/texel    */
#define CAM 256.0f
#define FAR_ALL 384.0f
#define BOUND 256.0f
#define REPS 20

static const ShLight LIGHT = {256.0f, 80.0f, 200.0f, 1.0f};
static const ShSeg SEGS[6] = {
    {150.0f, 300.0f, 360.0f, 300.0f},   /* near wall, d ~ 60..110   */
    {120.0f, 200.0f, 200.0f, 280.0f},   /* near diagonal            */
    {320.0f, 200.0f, 400.0f, 280.0f},   /* near diagonal            */
    {60.0f, 460.0f, 260.0f, 470.0f},    /* far wall,  d ~ 280       */
    {300.0f, 470.0f, 460.0f, 450.0f},   /* far wall,  d ~ 250..290  */
    {40.0f, 380.0f, 120.0f, 420.0f}     /* far wall,  d ~ 250       */
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static void rmse_bands(const ShadowMap *ref, const ShadowMap *got,
                       float *all, float *near, float *far) {
    double sa = 0.0, sn = 0.0, sf = 0.0;
    long na = 0, nn = 0, nf = 0;
    for (int y = 0; y < OUT; ++y) {
        float wy = (float)y + 0.5f;
        for (int x = 0; x < OUT; ++x) {
            float wx = (float)x + 0.5f;
            float d = hypotf(wx - CAM, wy - CAM);
            size_t i = (size_t)y * OUT + x;
            double e = (double)got->buf[i] - ref->buf[i];
            sa += e * e; ++na;
            if (d < BOUND) { sn += e * e; ++nn; }
            else           { sf += e * e; ++nf; }
        }
    }
    *all  = (float)sqrt(sa / na);
    *near = nn ? (float)sqrt(sn / nn) : 0.0f;
    *far  = nf ? (float)sqrt(sf / nf) : 0.0f;
}

static void run_cascade(const char *name, const ShTierCfg *cfg, int ntiers,
                        const ShadowMap *ref) {
    ShCascade c;
    int rc = sh_csm_build(&c, CAM, CAM, cfg, ntiers);
    if (rc != SH_OK) { printf("build failed: %s\n", sh_csm_strerror(rc)); return; }

    ShCsmStats rst = {0}, cst = {0};
    double t0 = now_ms();
    for (int r = 0; r < REPS; ++r) {
        rst.ray_tests = rst.texels = 0;
        sh_csm_render(&c, &LIGHT, SEGS, 6, &rst);
    }
    double render_ms = (now_ms() - t0) / REPS;

    ShadowMap out;
    sh_init(&out, OUT, OUT);
    t0 = now_ms();
    for (int r = 0; r < REPS; ++r)
        sh_csm_compose(&c, &out, 0.0f, 0.0f, (float)OUT, (float)OUT, &cst);
    double compose_ms = (now_ms() - t0) / REPS;

    float ea, en, ef;
    rmse_bands(ref, &out, &ea, &en, &ef);
    printf("%-18s rays=%-9ld texels=%-9ld render=%6.2fms compose=%5.2fms"
           "  RMSE all=%.4f near=%.4f far=%.4f uncovered=%ld\n",
           name, rst.ray_tests, rst.texels, render_ms, compose_ms,
           ea, en, ef, cst.uncovered / REPS);

    if (ntiers == 2) sh_write_pgm(&out, "demo.pgm");
    sh_free(&out);
    sh_csm_free(&c);
}

int main(void) {
    /* ground truth: direct evaluation at every output texel over [0,512]^2 */
    ShadowMap ref;
    sh_init(&ref, OUT, OUT);
    double t0 = now_ms();
    for (int r = 0; r < REPS; ++r)
        sh_render(&ref, &LIGHT, SEGS, 6);
    double gt_ms = (now_ms() - t0) / REPS;
    sh_write_pgm(&ref, "demo_ref.pgm");

    printf("scene: light(%.0f,%.0f) r=%.0f, %d segments, output %dx%d over "
           "[0,%d]^2 world units\n",
           LIGHT.x, LIGHT.y, LIGHT.radius, 6, OUT, OUT, OUT);
    printf("boundary rule: tier i owns [near, far); a point exactly on a\n"
           "boundary belongs to the farther tier.\n");
    printf("ground truth sh_render %dx%d: %.2fms/frame\n\n", OUT, OUT, gt_ms);

    ShTierCfg single256[1]  = {{0.0f, FAR_ALL, 256}};
    ShTierCfg single1024[1] = {{0.0f, FAR_ALL, 1024}};
    ShTierCfg two[2] = {{0.0f, BOUND, 1024}, {BOUND, FAR_ALL, 256}};

    /* tier table is queryable: dump it before running */
    ShCascade c;
    sh_csm_build(&c, CAM, CAM, two, 2);
    const ShTier *tab; int n;
    sh_csm_table(&c, &tab, &n);
    printf("tier table (%d tiers):\n", n);
    for (int i = 0; i < n; ++i)
        printf("  tier %d: distance [%6.1f, %6.1f)  res=%4d  texel=%.2f wu\n",
               tab[i].index, tab[i].near, tab[i].far, tab[i].res, tab[i].texel);
    sh_csm_free(&c);
    printf("\n");

    run_cascade("single-tier 256", single256, 1, &ref);
    run_cascade("single-tier 1024", single1024, 1, &ref);
    run_cascade("two-tier 1024/256", two, 2, &ref);

    sh_free(&ref);
    return 0;
}
