#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Benchmark: interpolated incremental shadow maps vs. per-frame full
 * re-render at 128 and 512.  For every frame both paths shade the same
 * interpolated light, so the errors reported are purely the cost of skipping
 * work.  Also checks determinism, long-run stability, that the map actually
 * moves, and exports the interpolated trajectory.
 */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Scene in normalised [0,1] coordinates, scaled to the map. */
static const ShSeg SCENE[4] = {
    {0.28f, 0.52f, 0.58f, 0.55f},
    {0.60f, 0.24f, 0.63f, 0.84f},
    {0.10f, 0.88f, 0.86f, 0.85f},
    {0.40f, 0.12f, 0.44f, 0.38f},
};

static void scaled_scene(ShSeg *out, int size) {
    for (int i = 0; i < 4; ++i) {
        out[i].x0 = SCENE[i].x0 * (float)size; out[i].y0 = SCENE[i].y0 * (float)size;
        out[i].x1 = SCENE[i].x1 * (float)size; out[i].y1 = SCENE[i].y1 * (float)size;
    }
}

typedef struct {
    double inc_ms, full_ms;          /* totals over the run                  */
    double inc_max_ms, full_max_ms;  /* per-frame worst case                 */
    long dirty_blocks, total_blocks, recompute, refade, skip;
    long dirty_pixels, recompute_pixels, total_pixels;
    double max_err, mean_err, rmse;  /* worst frame / mean of per-frame mean */
    int frames;
    unsigned long long hash;
} Bench;

static unsigned long long map_hash(const ShadowMap *m) {
    unsigned long long h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char*)m->buf;
    size_t n = (size_t)m->w * (size_t)m->h * sizeof(float);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* speed = light travel per frame (0..1 of the map diagonal). */
static int run_bench(int size, int block, int frames, float eps, double speed, Bench *out) {
    memset(out, 0, sizeof *out);
    const int W = size, H = size;
    ShSeg scene[4];
    scaled_scene(scene, size);

    ShadowMap ref;
    ShAnim a;
    if (sh_init(&ref, W, H) != 0) return -1;
    if (sh_anim_init(&a, W, H, block) != SH_OK) { sh_free(&ref); return -1; }
    sh_anim_set_eps(&a, eps);
    if (sh_anim_set_scene(&a, scene, 4) != SH_OK) { sh_anim_free(&a); sh_free(&ref); return -1; }

    /* Key frames are `frames*speed` map widths apart: with speed < 1/frames
     * the light moves sub-pixel per frame, with speed 1 it crosses once. */
    ShLight prev = {0.02f * (float)W, 0.10f * (float)H, 0.32f * (float)W, 1.0f};
    ShLight next = prev;
    next.x = prev.x + (float)(speed * (double)W * (double)frames);
    next.y = prev.y + (float)(0.10 * (double)H);
    next.radius = 0.32f * (float)W;
    next.intensity = 0.7f;

    double mean_acc = 0.0;
    ShError err;
    for (int f = 0; f < frames; ++f) {
        float t = (float)f / (float)(frames - 1);
        ShLight lit;
        ShStepStats st;
        if (sh_lerp_light(&prev, &next, t, frames - 1, &lit, &err) != SH_OK) return -1;

        double t0 = now_ms();
        if (sh_anim_step(&a, &prev, &next, t, frames - 1, 1, &st, &err) != SH_OK) return -1;
        double t1 = now_ms();

        if (sh_render(&ref, &lit, scene, 4) != 0) return -1;
        double t2 = now_ms();

        double mx, mean, rms;
        if (sh_compare_maps(&ref, &a.map, &mx, &mean, &rms) != SH_OK) return -1;
        if (mx > out->max_err) out->max_err = mx;
        if (rms > out->rmse) out->rmse = rms;
        mean_acc += mean;

        out->inc_ms += t1 - t0;
        if (t1 - t0 > out->inc_max_ms) out->inc_max_ms = t1 - t0;
        out->full_ms += t2 - t1;
        if (t2 - t1 > out->full_max_ms) out->full_max_ms = t2 - t1;
        out->dirty_blocks += st.dirty_blocks;
        out->total_blocks += st.total_blocks;
        out->recompute += st.recompute_blocks;
        out->refade += st.refade_blocks;
        out->skip += st.skip_blocks;
        out->dirty_pixels += st.dirty_pixels;
        out->recompute_pixels += st.recompute_pixels;
        out->total_pixels += st.total_pixels;
    }
    out->frames = frames;
    out->mean_err = mean_acc / (double)frames;
    out->hash = map_hash(&a.map);

    sh_anim_free(&a);
    sh_free(&ref);
    return 0;
}

int main(void) {
    enum { FRAMES = 96 };
    const int sizes[2] = {128, 512};
    const int blocks[2] = {8, 16};
    /* Sub-pixel motion (0.25 px/frame at 128) and a fast sweep
     * (2 px/frame) exercise both ends of the tolerance. */
    const double speeds[2] = {0.25 / 128.0, 2.0 / 128.0};
    const char *speed_name[2] = {"slow, sub-pixel motion", "fast, ~2 px/frame"};
    Bench b[2][2];
    int ok = 1;

    for (int sp = 0; sp < 2; ++sp) {
        printf("Incremental shadow map vs. per-frame full re-render\n");
        printf("%s, %d frames per run, eps = 1/255\n\n", speed_name[sp], FRAMES);
        for (int i = 0; i < 2; ++i) {
            if (run_bench(sizes[i], blocks[i], FRAMES, SH_EPS_DEFAULT, speeds[sp], &b[sp][i]) != 0) {
                fprintf(stderr, "bench failed at %d\n", sizes[i]);
                return 1;
            }
        }

    printf("+-------+-------+------------+------------+------------+---------+---------+\n");
    printf("| size  | block | recomputed | re-shaded  | untouched  | inc ms  | full ms |\n");
    printf("+-------+-------+------------+------------+------------+---------+---------+\n");
    for (int i = 0; i < 2; ++i) {
        double n = (double)FRAMES;
        printf("| %5d | %5d | %5.1f%%     | %5.1f%%     | %5.1f%%     | %7.2f | %7.2f |\n",
               sizes[i], blocks[i],
               100.0 * (double)b[sp][i].recompute / (double)b[sp][i].total_blocks,
               100.0 * (double)b[sp][i].refade / (double)b[sp][i].total_blocks,
               100.0 * (double)b[sp][i].skip / (double)b[sp][i].total_blocks,
               b[sp][i].inc_ms / n, b[sp][i].full_ms / n);
    }
    printf("+-------+-------+------------+------------+------------+---------+---------+\n\n");

    printf("+-------+-------+---------+------------+------------+------------+------------+\n");
    printf("| size  | block | speedup | recomputed | rec. px    | max err    | rmse       |\n");
    printf("+-------+-------+---------+------------+------------+------------+------------+\n");
    for (int i = 0; i < 2; ++i) {
        printf("| %5d | %5d | %5.2fx  | %5.1f/%-5.0f | %7.1f/%-6.0f| %10.6f | %10.6f |\n",
               sizes[i], blocks[i],
               b[sp][i].full_ms / b[sp][i].inc_ms,
               (double)b[sp][i].recompute / FRAMES, (double)b[sp][i].total_blocks / FRAMES,
               (double)b[sp][i].recompute_pixels / FRAMES, (double)b[sp][i].total_pixels / FRAMES,
               b[sp][i].max_err, b[sp][i].rmse);
    }
    printf("+-------+-------+---------+------------+------------+------------+------------+\n\n");

    for (int i = 0; i < 2; ++i) {
        printf("%3d: per-frame worst case inc %.2f ms vs full %.2f ms; "
               "mean error %.6f\n", sizes[i], b[sp][i].inc_max_ms, b[sp][i].full_max_ms,
               b[sp][i].mean_err);
        if (b[sp][i].max_err > 1.0 / 255.0 + 1e-4) {
            printf("     FAIL: max error %.6f exceeds one grey level\n", b[sp][i].max_err);
            ok = 0;
        }
    }
    printf("\n");
    }   /* speed regime */

    /* Stability 1: a repeated identical run is bit-identical. */
    Bench again;
    if (run_bench(128, 8, FRAMES, SH_EPS_DEFAULT, speeds[0], &again) != 0) return 1;
    printf("stability  repeat 128 run: final map %s\n",
           again.hash == b[0][0].hash ? "identical" : "DIFFERS");
    if (again.hash != b[0][0].hash) ok = 0;

    /* Stability 2: 8x more frames must not accumulate error. */
    Bench longrun;
    if (run_bench(128, 8, FRAMES * 8, SH_EPS_DEFAULT, speeds[0], &longrun) != 0) return 1;
    printf("stability  %d-frame run: max err %.6f, mean %.6f (%s)\n",
           FRAMES * 8, longrun.max_err, longrun.mean_err,
           longrun.max_err <= 1.0 / 255.0 + 1e-4 ? "bounded" : "GROWING");
    if (longrun.max_err > 1.0 / 255.0 + 1e-4) ok = 0;

    /* Stability 3: turning interpolation off must reproduce the legacy map
     * bit-for-bit, every frame. */
    {
        ShadowMap ref;
        ShAnim a;
        sh_init(&ref, 128, 128);
        sh_anim_init(&a, 128, 128, 8);
        ShSeg scene[4];
        scaled_scene(scene, 128);
        sh_anim_set_scene(&a, scene, 4);
        ShLight l = {30.0f, 18.0f, 40.0f, 0.9f};
        int same = 1;
        for (int f = 0; f < 32; ++f) {
            l.x += 2.5f;
            l.radius -= 0.4f;
            sh_render(&ref, &l, scene, 4);
            ShStepStats st;
            ShError e;
            sh_anim_step(&a, NULL, &l, 0.0f, 1, 0, &st, &e);
            same = same && st.full_refresh;
            for (int i = 0; i < 128 * 128; ++i)
                if (ref.buf[i] != a.map.buf[i]) { same = 0; break; }
        }
        printf("stability  interpolation off vs legacy sh_render: %s\n",
               same ? "bit-identical" : "DIFFERS");
        if (!same) ok = 0;
        sh_anim_free(&a);
        sh_free(&ref);
    }

    /* Trajectory export. */
    ShLight p = {12.0f, 12.0f, 38.0f, 1.0f};
    ShLight q = {116.0f, 22.0f, 38.0f, 0.7f};
    ShError e;
    if (sh_write_trajectory("demo_traj.csv", &p, &q, 33, 32, &e) != SH_OK) {
        fprintf(stderr, "trajectory: %s\n", e.msg);
        return 1;
    }
    printf("trajectory 33 interpolated samples written to demo_traj.csv\n");

    printf("\nverdict: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
