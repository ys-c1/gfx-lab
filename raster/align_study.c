/* Half-pixel vertex alignment: jitter, coverage and cost study.
 *
 * The leading edge of a thin triangle is the pixel column where its coverage
 * mask starts. When a vertex moves by a fraction of a pixel, the per-pixel
 * float edge test flips that column as soon as the edge crosses a pixel
 * centre, which is the visible crawl. Snapping vertices onto a 0.5 px lattice
 * before the scan pins the edge for motion smaller than half the grid.
 *
 * Every scenario is rendered twice with the same triangles:
 *   legacy  - rt_triangle, vertices consumed as given
 *   aligned - rt_triangle_aligned, grid RT_ALIGN_HALF_PX
 *
 * Reported per resolution:
 *   jitter     mean over rows of the temporal standard deviation of the
 *              row's leading covered pixel, in px. The triangle is static,
 *              so the ideal value is 0: this is the jitter amplitude error.
 *   flicker    mean pixels per frame whose coverage differs from frame 0.
 *   moved      fraction of frame pairs whose mask changed at all.
 *   edge_err   RMS distance (px) between the rendered leading edge and the
 *              analytic edge of the unsnapped triangle.
 *   cov_avg    mean covered pixels per frame on the smooth sweep.
 *   cov_dlt    mean |aligned - legacy| covered pixels over the same sweep.
 *   ns/tri     raster cost per triangle from a 32-triangle scene rendered
 *              into pre-reset targets; aligned includes the snap step. The
 *              scene covers slightly more pixels after snapping, so ns/px
 *              divides by the pixels actually drawn and is the fair number.
 *   ref_diff   pixels differing from a brute-force per-pixel reference on the
 *              vertices actually scanned. Must be 0 for both paths.
 *   max_off    largest |snapped - input| reported. Must be <= grid/2.
 *
 * Scenarios:
 *   A  static triangle, vertex noise +/-0.10 px (inside the snap cell)
 *   B  static triangle, vertex noise +/-0.20 px (crosses snap boundaries)
 *   C  smooth sub-pixel sweep: coverage, reference, cost
 */

#define _POSIX_C_SOURCE 200809L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAMES 96
#define SCENE_TRIS 32
#define TARGET_POOL 32
#define SAMPLES 7

typedef struct {
    double row_sd;
    double flicker;
    double moved;
    double edge_err;
    double cov_mean, cov_min, cov_max;
    double cov_delta;
    double ns_per_tri;
    double scene_work;        /* pixels drawn per target by the cost scene */
    double ref_diff;
    double max_off;
    int ok;
} Study;

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static double edge_fn(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Brute force: every pixel centre tested directly, no bbox shortcut. */
static int brute_force(const RTri *tri, int w, int h, unsigned char *mask) {
    int cov = 0;
    memset(mask, 0, (size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge_fn(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge_fn(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge_fn(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            mask[(size_t)y * w + x] = 1;
            ++cov;
        }
    return cov;
}

static void target_mask(const RTarget *t, unsigned char *mask, int w, int h) {
    for (int i = 0; i < w * h; ++i)
        mask[i] = (t->rgb[i*3] | t->rgb[i*3+1] | t->rgb[i*3+2]) ? 1 : 0;
}

static void reset_target(RTarget *t) {
    int n = t->w * t->h;
    for (int i = 0; i < n; ++i) { t->z[i] = 1e30f; t->rgb[i*3] = t->rgb[i*3+1] = t->rgb[i*3+2] = 0; }
}

static double leading(const unsigned char *m, int w, int y) {
    for (int x = 0; x < w; ++x) if (m[(size_t)y * w + x]) return x + 0.5;
    return NAN;
}

/* Thin triangle: 2 px across at the top, 76% of the target tall, tilted. */
static RTri make_tri(int h, double x, double y) {
    const double thick = 2.0;
    RTri t = {{ { x,               y,         0.5 },
                { x + thick,       y + 2.5,   0.5 },
                { x + thick * 0.4, h * 0.88,  0.5 } },
              {220, 60, 40} };
    return t;
}

static unsigned int rng_state = 0x9e3779b9u;
static double noise(double amp) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return ((double)(rng_state & 0xffffu) / 65535.0 * 2.0 - 1.0) * amp;
}

/* Static scenario with per-frame vertex noise. */
static Study study_static(int w, int h, int use_align, double amp) {
    Study st;
    memset(&st, 0, sizeof(st));
    unsigned char *base = (unsigned char*)malloc((size_t)w * h);
    unsigned char *cur = (unsigned char*)malloc((size_t)w * h);
    double sum[2048], sum2[2048];
    int cnt[2048];
    int moved = 0;
    double flick = 0.0, edge_sq = 0.0;
    int edge_n = 0;
    double cov_sum = 0.0, cov_min = 1e30, cov_max = -1e30;
    RTAlign align = {1, RT_ALIGN_HALF_PX};
    RTAlignReport rep;
    RTarget t;
    if (!base || !cur || rt_init(&t, w, h) != 0) { free(base); free(cur); return st; }
    memset(sum, 0, sizeof(sum));
    memset(sum2, 0, sizeof(sum2));
    memset(cnt, 0, sizeof(cnt));

    rng_state = 0x9e3779b9u;
    const double bx = (double)w * 0.5 + 0.13;
    const double by = (double)h * 0.12 + 0.17;

    for (int f = 0; f < FRAMES; ++f) {
        RTri tri = make_tri(h, bx, by);
        for (int i = 0; i < 3; ++i) {
            tri.v[i].x += noise(amp);
            tri.v[i].y += noise(amp);
        }
        reset_target(&t);
        int rc = use_align ? rt_triangle_aligned(&t, &tri, &align, &rep) : rt_triangle(&t, &tri);
        if (rc < 0) { free(base); free(cur); rt_free(&t); return st; }
        if (use_align && rep.max_displacement > st.max_off) st.max_off = rep.max_displacement;
        target_mask(&t, cur, w, h);

        int cov = 0, diff = 0;
        for (int i = 0; i < w * h; ++i) { cov += cur[i]; if (f > 0 && cur[i] != base[i]) ++diff; }
        cov_sum += cov;
        if (cov < cov_min) cov_min = cov;
        if (cov > cov_max) cov_max = cov;
        if (f > 0) { flick += diff; if (diff) ++moved; }
        if (f == 0) memcpy(base, cur, (size_t)w * h);

        for (int y = 0; y < h; ++y) {
            double l = leading(cur, w, y);
            if (isnan(l)) continue;
            sum[y] += l; sum2[y] += l * l; ++cnt[y];
            double py = y + 0.5;
            double ya = tri.v[0].y, yb = tri.v[2].y;
            if (fabs(yb - ya) < 1e-12) continue;
            double xe = tri.v[0].x + (py - ya) * (tri.v[2].x - tri.v[0].x) / (yb - ya);
            if (xe < 0 || xe > w) continue;
            double d = l - xe;
            edge_sq += d * d; ++edge_n;
        }
    }

    double sd_sum = 0.0;
    int sd_n = 0;
    for (int y = 0; y < h && y < 2048; ++y) {
        if (cnt[y] < 2) continue;
        double mu = sum[y] / cnt[y];
        double var = sum2[y] / cnt[y] - mu * mu;
        if (var < 1e-18) continue;
        sd_sum += sqrt(var); ++sd_n;
    }
    st.row_sd = sd_n ? sd_sum / sd_n : 0.0;
    st.flicker = flick / (FRAMES - 1);
    st.moved = (double)moved / (FRAMES - 1);
    st.edge_err = edge_n ? sqrt(edge_sq / edge_n) : NAN;
    st.cov_mean = cov_sum / FRAMES;
    st.cov_min = cov_min;
    st.cov_max = cov_max;
    st.ns_per_tri = NAN;
    st.ref_diff = NAN;
    st.ok = 1;
    rt_free(&t);
    free(base); free(cur);
    return st;
}

/* Scene cost: SCENE_TRIS thin triangles spread over the target, drawn into
 * pre-reset targets from a pool. Reset is untimed, both paths draw the same
 * scene, and the fastest sample wins. Aligned includes the snap step. */
static double scene_work_legacy = 0.0, scene_work_aligned = 0.0;

static double scene_ns_per_tri(int w, int h, int use_align) {
    RTarget pool[TARGET_POOL];
    RTri scene[SCENE_TRIS];
    RTAlign align = {1, RT_ALIGN_HALF_PX};
    RTAlignReport rep;
    int made = 0;
    for (int i = 0; i < TARGET_POOL; ++i) {
        if (rt_init(&pool[i], w, h) != 0) break;
        ++made;
    }
    if (made != TARGET_POOL) {
        for (int i = 0; i < made; ++i) rt_free(&pool[i]);
        return -1;
    }

    /* Eight tall triangles, each one screen column wide at the base and
     * spanning 70% of the target height, spread across the width. Their area
     * is large enough that the snap step is measured against real scan work
     * rather than against a two-pixel bbox. */
    for (int i = 0; i < SCENE_TRIS; ++i) {
        int gx = i % 8, gy = i / 8;                 /* 8 x 4 cells */
        double cell_w = (double)w / 8.0;
        double cell_h = (double)h / 4.0;
        double x = (double)gx * cell_w + cell_w * 0.25 + 0.13 * (double)(i % 5);
        double y = (double)gy * cell_h + cell_h * 0.10 + 0.09 * (double)(i % 3);
        double tall = cell_h * 0.70;
        double slope = (((i / 8) & 1) ? -1.0 : 1.0) * 0.35;   /* alternate lean */
        const double thick = 1.25;
        scene[i].v[0] = (RVert){ x,                 y,        0.5 };
        scene[i].v[1] = (RVert){ x + thick,         y + 2.0,  0.5 };
        scene[i].v[2] = (RVert){ x + thick * 0.4 + slope * tall, y + tall, 0.5 };
        scene[i].rgb[0] = 220; scene[i].rgb[1] = 60; scene[i].rgb[2] = 40;
    }

    /* Pixels actually drawn by the scene: must be equal for both paths. */
    double covered = 0.0;
    for (int i = 0; i < TARGET_POOL; ++i) {
        reset_target(&pool[i]);
        for (int k = 0; k < SCENE_TRIS; ++k) {
            if (use_align) rt_triangle_aligned(&pool[i], &scene[k], &align, &rep);
            else rt_triangle(&pool[i], &scene[k]);
        }
        for (int j = 0; j < w * h; ++j) covered += pool[i].rgb[j*3] ? 1 : 0;
    }
    covered /= TARGET_POOL;
    if (getenv("GFXLAB_VERBOSE"))
        fprintf(stderr, "scene %dx%d %s covers %.0f px\n", w, h,
                use_align ? "aligned" : "legacy", covered);

    if (use_align) scene_work_aligned = covered; else scene_work_legacy = covered;

    double best = 1e30;
    for (int s = 0; s < SAMPLES; ++s) {
        for (int i = 0; i < TARGET_POOL; ++i) reset_target(&pool[i]);  /* untimed */
        double t0 = now_ns();
        for (int i = 0; i < TARGET_POOL; ++i)
            for (int k = 0; k < SCENE_TRIS; ++k) {
                if (use_align) rt_triangle_aligned(&pool[i], &scene[k], &align, &rep);
                else rt_triangle(&pool[i], &scene[k]);
            }
        double dt = (now_ns() - t0) / ((double)TARGET_POOL * SCENE_TRIS);
        if (dt < best) best = dt;
    }
    for (int i = 0; i < TARGET_POOL; ++i) rt_free(&pool[i]);
    return best;
}

/* Smooth sweep: coverage, reference agreement, jitter under real motion. */
static Study study_sweep(int w, int h, int use_align) {
    Study st;
    memset(&st, 0, sizeof(st));
    unsigned char *cur = (unsigned char*)malloc((size_t)w * h);
    unsigned char *prev = (unsigned char*)malloc((size_t)w * h);
    unsigned char *ref = (unsigned char*)malloc((size_t)w * h);
    RTAlign align = {1, RT_ALIGN_HALF_PX};
    RTAlignReport rep;
    RTarget t;
    if (!cur || !prev || !ref || rt_init(&t, w, h) != 0) {
        free(cur); free(prev); free(ref); return st;
    }

    const double x0 = (double)w * 0.35 + 0.07;
    const double travel = 4.0 * RT_ALIGN_HALF_PX;
    double cov_sum = 0.0, cov_min = 1e30, cov_max = -1e30;
    double ref_sum = 0.0, flick = 0.0;
    int moved = 0;

    for (int f = 0; f < FRAMES; ++f) {
        double phase = travel * (double)f / (double)(FRAMES - 1);
        RTri tri = make_tri(h, x0 + phase, (double)h * 0.12 + 0.17);
        RTri scanned = tri;
        if (use_align) {
            if (rt_align_vertices(&tri, &align, &scanned, &rep) != RT_OK) {
                free(cur); free(prev); free(ref); rt_free(&t); return st;
            }
            if (rep.max_displacement > st.max_off) st.max_off = rep.max_displacement;
        }

        reset_target(&t);
        int rc = use_align ? rt_triangle_aligned(&t, &tri, &align, &rep) : rt_triangle(&t, &tri);
        if (rc < 0) { free(cur); free(prev); free(ref); rt_free(&t); return st; }
        target_mask(&t, cur, w, h);

        int cov = 0, diff = 0;
        for (int i = 0; i < w * h; ++i) { cov += cur[i]; if (f > 0 && cur[i] != prev[i]) ++diff; }
        cov_sum += cov;
        if (cov < cov_min) cov_min = cov;
        if (cov > cov_max) cov_max = cov;
        if (f > 0) { flick += diff; if (diff) ++moved; }

        brute_force(&scanned, w, h, ref);
        int d = 0;
        for (int i = 0; i < w * h; ++i) if (cur[i] != ref[i]) ++d;
        ref_sum += d;
        memcpy(prev, cur, (size_t)w * h);
    }

    st.cov_mean = cov_sum / FRAMES;
    st.cov_min = cov_min;
    st.cov_max = cov_max;
    st.flicker = flick / (FRAMES - 1);
    st.moved = (double)moved / (FRAMES - 1);
    st.ref_diff = ref_sum / FRAMES;
    st.ok = 1;
    rt_free(&t);
    free(cur); free(prev); free(ref);
    return st;
}

/* Mean |aligned - legacy| coverage over the sweep, at identical phases. */
static double sweep_coverage_delta(int w, int h) {
    unsigned char *a = (unsigned char*)malloc((size_t)w * h);
    unsigned char *b = (unsigned char*)malloc((size_t)w * h);
    RTAlign align = {1, RT_ALIGN_HALF_PX};
    RTAlignReport rep;
    RTarget ta, tb;
    if (!a || !b || rt_init(&ta, w, h) != 0 || rt_init(&tb, w, h) != 0) {
        free(a); free(b); return -1;
    }
    double sum = 0.0;
    double x0 = (double)w * 0.35 + 0.07, travel = 4.0 * RT_ALIGN_HALF_PX;
    for (int f = 0; f < FRAMES; ++f) {
        double phase = travel * (double)f / (double)(FRAMES - 1);
        RTri tri = make_tri(h, x0 + phase, (double)h * 0.12 + 0.17);
        reset_target(&ta); reset_target(&tb);
        rt_triangle(&ta, &tri);
        rt_triangle_aligned(&tb, &tri, &align, &rep);
        target_mask(&ta, a, w, h);
        target_mask(&tb, b, w, h);
        int ca = 0, cb = 0;
        for (int i = 0; i < w * h; ++i) { ca += a[i]; cb += b[i]; }
        sum += fabs((double)(cb - ca));
    }
    rt_free(&ta); rt_free(&tb);
    free(a); free(b);
    return sum / FRAMES;
}

static void row(int w, const char *mode, const Study *a, const Study *s, double legacy_ns) {
    double per_px = s->scene_work > 0 ? s->ns_per_tri / (s->scene_work / SCENE_TRIS) : 0.0;
    printf("%-4d %-9s %8.3f %8.2f %7.1f%% %8.3f %8.1f %7.0f %7.0f %8.2f %8.0f %7.1f%% %8.2f %8.2f\n",
           w, mode, a->row_sd, a->flicker, a->moved * 100.0, a->edge_err,
           s->cov_mean, s->cov_min, s->cov_max, s->cov_delta, s->ns_per_tri,
           legacy_ns > 0 ? (s->ns_per_tri - legacy_ns) * 100.0 / legacy_ns : 0.0,
           s->ref_diff, per_px);
}

int main(void) {
    printf("raster half-pixel alignment study (grid %.2f px)\n", RT_ALIGN_HALF_PX);
    printf("A: static, vertex noise +/-0.10 px   B: static, vertex noise +/-0.20 px\n");
    printf("C: smooth sweep over 4 grid cells\n");
    printf("%d frames per scenario; cost from a %d-triangle scene, best of %d samples\n\n",
           FRAMES, SCENE_TRIS, SAMPLES);
    printf("%-4s %-9s %8s %8s %8s %8s %8s %7s %7s %8s %8s %8s %8s %8s\n",
           "res", "mode", "jitter", "flicker", "moved", "edge_err",
           "cov_avg", "cov_min", "cov_max", "cov_dlt", "ns/tri", "cost",
           "ref_diff", "ns/px");
    const int sizes[2] = {32, 128};
    for (int i = 0; i < 2; ++i) {
        int w = sizes[i];
        Study la = study_static(w, w, 0, 0.10);
        Study aa = study_static(w, w, 1, 0.10);
        Study lb = study_static(w, w, 0, 0.20);
        Study ab = study_static(w, w, 1, 0.20);
        Study ls = study_sweep(w, w, 0);
        Study as = study_sweep(w, w, 1);
        double delta = sweep_coverage_delta(w, w);
        ls.ns_per_tri = scene_ns_per_tri(w, w, 0);
        ls.scene_work = scene_work_legacy;
        as.ns_per_tri = scene_ns_per_tri(w, w, 1);
        as.scene_work = scene_work_aligned;
        ls.cov_delta = as.cov_delta = delta;
        if (aa.max_off > as.max_off) as.max_off = aa.max_off;
        if (ab.max_off > as.max_off) as.max_off = ab.max_off;

        printf("-- noise +/-0.10 px\n");
        row(w, "legacy", &la, &ls, 0.0);
        row(w, "aligned", &aa, &as, ls.ns_per_tri);
        printf("-- noise +/-0.20 px\n");
        row(w, "legacy", &lb, &ls, 0.0);
        row(w, "aligned", &ab, &as, ls.ns_per_tri);
        printf("-- jitter amplitude error: legacy %.3f/%.3f px -> aligned %.3f/%.3f px (%.1f%% / %.1f%% reduction)\n",
               la.row_sd, lb.row_sd, aa.row_sd, ab.row_sd,
               la.row_sd > 0 ? (1.0 - aa.row_sd / la.row_sd) * 100.0 : 0.0,
               lb.row_sd > 0 ? (1.0 - ab.row_sd / lb.row_sd) * 100.0 : 0.0);
        printf("-- edge error change: %+.3f px at +/-0.10, %+.3f px at +/-0.20\n",
               aa.edge_err - la.edge_err, ab.edge_err - lb.edge_err);
        printf("-- coverage: aligned %+.1f%% mean, spread %d..%d vs legacy %d..%d px\n",
               (as.cov_mean - ls.cov_mean) * 100.0 / ls.cov_mean,
               (int)as.cov_min, (int)as.cov_max, (int)ls.cov_min, (int)ls.cov_max);
        printf("-- max |offset| %.3f px; reference agreement %s\n", as.max_off,
               (as.ref_diff == 0.0 && ls.ref_diff == 0.0) ? "exact" : "MISMATCH");
        printf("-- cost scene draws %.0f px (aligned) vs %.0f px (legacy); ns/px normalizes that\n\n",
               as.scene_work, ls.scene_work);
    }
    return 0;
}
