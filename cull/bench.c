#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define FRAMES 61            /* t = 0, 1/60, ..., 1 */
#define KEYFRAMES 5          /* t = 0, .25, .5, .75, 1 (hand table sampling) */
#define REPEAT 5

typedef struct {
    CullBox *boxes; int n;
    CullView v0, v1;
    const char *name;
} Scene;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int run_mode(const Scene *sc, int flags, int frames, double *elapsed_out,
                    int *rebuilds_out, CullAnim *keep) {
    double best = 1e300;
    CullStepInfo f;
    for (int rep = 0; rep < REPEAT; ++rep) {
        CullAnim a;
        double t0 = now_sec();
        int rc = cull_anim_init(&a, sc->boxes, sc->n, sc->v0, sc->v1, flags, &f);
        if (rc) return rc;
        for (int k = 1; k < frames; ++k) {
            rc = cull_anim_step(&a, (double)k / (frames - 1), &f);
            if (rc) { cull_anim_free(&a); return rc; }
        }
        double dt = now_sec() - t0;
        if (dt < best) best = dt;
        *rebuilds_out = a.rebuilds;
        if (keep) { *keep = a; break; }   /* export run: single pass */
        cull_anim_free(&a);
    }
    *elapsed_out = best;
    return 0;
}

/* run both modes on one scene and verify frame-by-frame equality.
   suffix names the exported files; tag is printed in the header. */
static int bench_scene(const Scene *sc, int frames, const char *suffix,
                       const char *outdir) {
    double t_inc, t_full;
    int rb_inc, rb_full;
    CullAnim a_inc, a_full;

    int rc = run_mode(sc, 0, frames, &t_inc, &rb_inc, &a_inc);
    if (rc) { printf("init error: %s\n", cull_strerror(rc)); return rc; }
    rc = run_mode(sc, CULL_ANIM_FULL_REBUILD, frames, &t_full, &rb_full, &a_full);
    if (rc) { cull_anim_free(&a_inc); return rc; }

    /* cross-check the two modes on every stored frame */
    int errors = 0, total_ids = 0;
    if (a_inc.nhist != a_full.nhist) { ++errors; }
    int nf = a_inc.nhist < a_full.nhist ? a_inc.nhist : a_full.nhist;
    for (int i = 0; i < nf; ++i) {
        const CullFrameRecord *p = &a_inc.hist[i];
        const CullFrameRecord *q = &a_full.hist[i];
        if (p->visible_count != q->visible_count) { ++errors; continue; }
        total_ids += p->visible_count;
        for (int j = 0; j < p->visible_count; ++j)
            if (p->visible_ids[j] != q->visible_ids[j]) ++errors;
    }

    char path[256];
    snprintf(path, sizeof path, "%s/%s%s_traj_inc.csv", outdir, sc->name, suffix);
    cull_anim_export_csv(&a_inc, path);
    snprintf(path, sizeof path, "%s/%s%s_traj_full.csv", outdir, sc->name, suffix);
    cull_anim_export_csv(&a_full, path);
    snprintf(path, sizeof path, "%s/%s_boxes.csv", outdir, sc->name);
    cull_anim_export_boxes(sc->boxes, sc->n, path);

    printf("==== scene: %s  (n=%d boxes, %d frames) ====\n",
           sc->name, sc->n, frames);
    printf("candidate set size      : %d (%.3f%% of n)\n",
           a_inc.ncand, 100.0 * a_inc.ncand / sc->n);
    printf("rebuilds  incremental   : %d   (full-scan frames)\n", rb_inc);
    printf("rebuilds  full-rebuild  : %d\n", rb_full);
    printf("elapsed   incremental   : %.6f s (best of %d)\n", t_inc, REPEAT);
    printf("elapsed   full-rebuild  : %.6f s (best of %d)\n", t_full, REPEAT);
    printf("speedup                 : %.3fx\n", t_full / t_inc);
    printf("cross-mode id mismatches: %d over %d visible ids -> error %.3e\n",
           errors, total_ids, total_ids ? (double)errors / total_ids : 0.0);
    printf("per-frame report (incremental; full rebuilds=%d every frame):\n",
           rb_full);
    printf("  frame        t  candidates rebuilt added removed unchanged visible\n");
    for (int i = 0; i < nf; ++i) {
        const CullFrameRecord *r = &a_inc.hist[i];
        if (sc->n > 1000 && i > 3 && i < nf - 3) continue;  /* big scene: head+tail */
        printf("  %5d %8.4f  %10d %7d %5d %7d %9d %7d\n",
               r->frame, r->t, r->candidates, r->rebuilt,
               r->added, r->removed, r->unchanged, r->visible_count);
    }
    if (sc->n > 1000) printf("  ... (%d frames total, see exported csv) ...\n", nf);

    cull_anim_free(&a_inc);
    cull_anim_free(&a_full);
    return errors ? -1 : 0;
}

int main(void) {
    /* ---- small scene: identical to the hand-check table ---- */
    CullBox small[6] = {
        {0,0,8,8,0},{0,15,8,8,0},{27,0,8,8,1},{35,0,8,8,2},
        {80,80,8,8,0},{-25,-25,8,8,1}};
    Scene s_small = { small, 6, {0,0,30,30}, {40,0,30,30}, "small" };

    /* ---- large scene: 100k grid boxes, panning 100px viewport ---- */
    enum { GW = 1000, GH = 100, N = GW * GH };
    CullBox *big = malloc(sizeof(CullBox) * N);
    if (!big) return 2;
    for (int i = 0; i < N; ++i) {
        big[i].x = (i % GW) * 10.0;
        big[i].y = (i / GW) * 10.0;
        big[i].w = 8.0; big[i].h = 8.0;
        big[i].layer = i % 3;
    }
    Scene s_big = { big, N, {0,0,100,100}, {100,0,100,100}, "large" };

    const char *outdir = "build/benchout";
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return 2;
    }

    int rc1 = bench_scene(&s_small, FRAMES, "", outdir);
    int rc3 = bench_scene(&s_small, KEYFRAMES, "_key", outdir);
    int rc2 = bench_scene(&s_big, FRAMES, "", outdir);
    free(big);
    printf("\ntrajectory csv written under %s/\n", outdir);
    return (rc1 || rc2 || rc3) ? 1 : 0;
}
