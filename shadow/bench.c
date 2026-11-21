/* bench: low/high tiers vs exhaustive ground truth.
 * reports per-tier mask error, timing, direction bins, sample count and
 * per-pixel shadow rays; exports the direction-mask tables to CSV. */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 128
#define H 128
#define REF_SAMPLES 64
#define REF_BINS 32

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* independent brute-force ground truth: every sample ray per pixel.
 * same physical model as the renderer: mean emission-cosine-weighted visibility */
static void exhaustive(ShadowMap *m, const ShShapeCfg *cfg, const ShSeg *segs, int nseg) {
    int S = REF_SAMPLES;
    float ax = cosf(cfg->theta), ay = sinf(cfg->theta);
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float wvis = 0.0f;
            for (int s = 0; s < S; ++s) {
                float t = ((float)s + 0.5f) * 2.0f / (float)S - 1.0f;
                float ex = cfg->base.x + t * cfg->half_len * ax;
                float ey = cfg->base.y + t * cfg->half_len * ay;
                float rx = px - ex, ry = py - ey;
                float rlen = sqrtf(rx * rx + ry * ry);
                float cos_e = rlen > 1e-9f ? fabsf(ax * ry - ay * rx) / rlen : 0.0f;
                int occ = 0;
                for (int i = 0; i < nseg; ++i) {
                    float sx = segs[i].x1 - segs[i].x0, sy = segs[i].y1 - segs[i].y0;
                    float den = rx * sy - ry * sx;
                    if (fabsf(den) < 1e-9f) continue;
                    float tt = ((segs[i].x0 - ex) * sy - (segs[i].y0 - ey) * sx) / den;
                    float uu = ((segs[i].x0 - ex) * ry - (segs[i].y0 - ey) * rx) / den;
                    if (tt > 0.0f && tt < 1.0f && uu >= 0.0f && uu <= 1.0f) { occ = 1; break; }
                }
                if (!occ) wvis += cos_e / (float)S;
            }
            float dx = px - cfg->base.x, dy = py - cfg->base.y;
            float d = sqrtf(dx * dx + dy * dy);
            float fall = 1.0f / (1.0f + d / cfg->base.radius);
            m->buf[(size_t)y * m->w + x] = cfg->base.intensity * fall * wvis;
        }
    }
}

/* reference direction mask: REF_SAMPLES samples x REF_BINS bins, all selected */
static void ref_mask(const ShShapeCfg *cfg, ShDirMask *ref) {
    int B = REF_BINS, S = REF_SAMPLES;
    memset(ref, 0, sizeof(*ref));
    ref->bins = B; ref->samples = S; ref->theta = cfg->theta; ref->half_len = cfg->half_len;
    ref->sel = (unsigned char*)calloc((size_t)B * S, 1);
    ref->weight = (float*)calloc((size_t)B * S, sizeof(float));
    for (int b = 0; b < B; ++b)
        for (int s = 0; s < S; ++s) {
            ref->sel[(size_t)b * S + s] = 1;
            ref->weight[(size_t)b * S + s] = 1.0f / (float)S;
        }
}

/* mask RMSE: compare visibility implied by the tier mask against the
 * exhaustive mask, evaluated along every (bin, sample) cell; coarse bins are
 * expanded onto the fine reference grid by angular midpoint mapping. */
static double mask_rmse(const ShDirMask *tier, const ShDirMask *refm) {
    double acc = 0.0;
    int n = 0;
    for (int b = 0; b < refm->bins; ++b) {
        int cb = (int)((float)(b + 0.5f) / refm->bins * tier->bins);
        if (cb >= tier->bins) cb = tier->bins - 1;
        for (int s = 0; s < refm->samples; ++s) {
            /* fine sample index -> nearest coarse sample index */
            int cs = (int)(((float)s + 0.5f) / refm->samples * tier->samples);
            if (cs >= tier->samples) cs = tier->samples - 1;
            double pred = tier->sel[(size_t)cb * tier->samples + cs]
                        ? (double)tier->weight[(size_t)cb * tier->samples + cs] : 0.0;
            double truth = (double)refm->weight[(size_t)b * refm->samples + s];
            double e = pred - truth;
            acc += e * e; ++n;
        }
    }
    return sqrt(acc / (double)n);
}

static void rmse_map(const ShadowMap *a, const ShadowMap *b, double *rms, float *maxd) {
    double acc = 0.0; *maxd = 0.0f;
    int n = a->w * a->h;
    for (int i = 0; i < n; ++i) {
        float e = a->buf[i] - b->buf[i];
        acc += (double)e * e;
        if (fabsf(e) > *maxd) *maxd = fabsf(e);
    }
    *rms = sqrt(acc / (double)n);
}

static int run_tier(const char *name, const ShShapeCfg *base_cfg,
                    const ShSeg *segs, int nseg, const ShadowMap *truth,
                    const ShDirMask *refm, const char *csv_path) {
    ShShapeCfg cfg = *base_cfg;
    cfg.enable = 1;
    ShDirMask mk;
    if (sh_mask_build(&cfg, &mk) != 0) { printf("[%s] mask build failed: %s\n", name, sh_errmsg()); return 1; }
    if (sh_mask_write_csv(&mk, csv_path) != 0) { printf("[%s] csv write failed\n", name); return 1; }

    ShadowMap out;
    if (sh_init(&out, W, H) != 0) return 1;
    double t0 = now_sec();
    int rc = sh_render_shaped(&out, &cfg, segs, nseg);
    double t1 = now_sec();
    if (rc != 0) { printf("[%s] render failed: %s\n", name, sh_errmsg()); return 1; }

    double rms; float maxd;
    rmse_map(&out, truth, &rms, &maxd);
    double merr = mask_rmse(&mk, refm);
    int picks = 0;
    for (int s = 0; s < mk.samples; ++s) picks += mk.sel[s];
    long rays = (long)W * H * picks;

    printf("[%s] bins=%d samples=%d picks/bin=%d rays=%ld time_ms=%.3f "
           "mask_rmse=%.6f map_rmse=%.6f map_max=%.6f csv=%s\n",
           name, mk.bins, mk.samples, picks, rays, (t1 - t0) * 1e3,
           merr, rms, maxd, csv_path);
    sh_mask_free(&mk);
    sh_free(&out);
    return 0;
}

int main(void) {
    ShShapeCfg cfg = {0};
    cfg.base.x = 64.0f; cfg.base.y = 24.0f;
    cfg.base.radius = 48.0f; cfg.base.intensity = 1.0f;
    cfg.shape = SH_SHAPE_SEGMENT;
    cfg.theta = 0.35f;          /* tilted strip light */
    cfg.half_len = 16.0f;
    ShSeg segs[2] = {{24.0f, 70.0f, 66.0f, 70.0f}, {78.0f, 84.0f, 118.0f, 84.0f}};

    ShadowMap truth;
    if (sh_init(&truth, W, H) != 0) return 1;
    double t0 = now_sec();
    exhaustive(&truth, &cfg, segs, 2);
    double t1 = now_sec();
    if (sh_write_pgm(&truth, "build/bench_truth.pgm") != 0) return 1;

    ShDirMask refm;
    ref_mask(&cfg, &refm);
    if (sh_mask_write_csv(&refm, "build/mask_exhaustive.csv") != 0) return 1;

    printf("ground truth: samples=%d bins=%d time_ms=%.3f rays=%d\n",
           REF_SAMPLES, REF_BINS, (t1 - t0) * 1e3, W * H * REF_SAMPLES);

    ShShapeCfg low = cfg;  low.mask_mode = SH_MASK_STRATIFIED;  low.tier = SH_TIER_LOW;
    ShShapeCfg high = cfg; high.mask_mode = SH_MASK_STRATIFIED; high.tier = SH_TIER_HIGH;
    int rc = 0;
    rc |= run_tier("low ", &low,  segs, 2, &truth, &refm, "build/mask_low.csv");
    rc |= run_tier("high", &high, segs, 2, &truth, &refm, "build/mask_high.csv");

    /* sanity: exhaustive mask mode with a single bin evaluates every sample
       ray per pixel, so it must reproduce the independent brute-force truth
       bit for bit (same model, same sample order); binning error is what the
       low/high stratified tiers above are measured against. */
    ShShapeCfg exh = cfg;
    exh.mask_mode = SH_MASK_EXHAUSTIVE; exh.tier = SH_TIER_LOW;
    exh.bins = 1; exh.samples = REF_SAMPLES;
    ShadowMap eout;
    if (sh_init(&eout, W, H) != 0) return 1;
    if (sh_render_shaped(&eout, &exh, segs, 2) != 0) {
        printf("[exh ] render failed: %s\n", sh_errmsg());
        return 1;
    }
    double er; float em;
    rmse_map(&eout, &truth, &er, &em);
    printf("[exh ] bins=%d samples=%d map_rmse=%.8f map_max=%.8f (must be ~0)\n",
           exh.bins, exh.samples, er, em);
    if (er > 1e-6) rc = 1;

    sh_mask_free(&refm);
    sh_free(&truth); sh_free(&eout);
    printf(rc ? "BENCH FAILED\n" : "bench ok\n");
    return rc;
}
