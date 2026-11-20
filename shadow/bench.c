#define _POSIX_C_SOURCE 199309L
/* Benchmark: two-layer culling vs. brute force, with a hand-checked baseline.
 *
 *   ./build/bench 1000  [out.txt]
 *   ./build/bench 10000 [out.txt]
 *
 * Deterministic apart from the timing columns.  "hand" is an independent
 * brute-force reference that answers, per segment, the only question that
 * matters: "can this segment block any pixel centre?"  It tests every pixel
 * centre of the map against the segment with the same intersection predicate
 * the renderer uses.  Any segment that blocks at least one pixel must survive
 * culling; the benchmark fails if one does not.
 *
 * The full candidate table and the hand verdict per segment are written to
 * out.txt (default bench_<n>.txt).
 */
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAP_W 128
#define MAP_H 128

/* Light above the map: the frustum is a pentagon, not the plain map rect. */
static const ShLight LIGHT = {64.0f, -24.0f, 40.0f, 1.0f};

static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;
static unsigned rng_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 32);
}
static double rng_unit(void) { return (double)rng_u32() / 4294967296.0; }
static double rng_rng(double a, double b) { return a + (b - a) * rng_unit(); }

/* Same predicate as shadow.c, in double, on pixel centres. */
static int pixel_blocked(const ShSeg *s, double lx, double ly, double px, double py) {
    double rx = px - lx, ry = py - ly;
    double sx = (double)s->x1 - s->x0, sy = (double)s->y1 - s->y0;
    double den = rx * sy - ry * sx;
    if (fabs(den) < 1e-9) return 0;
    double t = (((double)s->x0 - lx) * sy - ((double)s->y0 - ly) * sx) / den;
    double u = (((double)s->x0 - lx) * ry - ((double)s->y0 - ly) * rx) / den;
    return t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0;
}

/* Independent baseline: does the segment block any pixel centre? */
static int hand_blocks_any(const ShadowMap *m, const ShSeg *s) {
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x)
            if (pixel_blocked(s, LIGHT.x, LIGHT.y, x + 0.5, y + 0.5)) return 1;
    return 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Scene mix, by construction:
 *   40% -- clutter inside the map                          (must be kept)
 *   10% -- just above the map, inside the light cone        (must be kept)
 *   50% -- far above the light or far to the sides, i.e.
 *          outside the frustum, so unable to reach a pixel (must be culled)
 * The light is at (64,-24); the cone opens downward through the map. */
static void make_scene(ShSeg *segs, int n) {
    for (int i = 0; i < n; ++i) {
        int kind = (int)(rng_u32() % 100);
        double x0, y0, x1, y1;
        if (kind < 40) {
            x0 = rng_rng(4, 124); y0 = rng_rng(4, 120);
            x1 = x0 + rng_rng(-16, 16); y1 = y0 + rng_rng(-6, 6);
        } else if (kind < 50) {
            x0 = rng_rng(50, 78); y0 = rng_rng(-18, -2);
            x1 = x0 + rng_rng(-4, 4); y1 = y0 + rng_rng(2, 12);
        } else if (kind < 75) {
            x0 = rng_rng(-300, 400); y0 = rng_rng(-400, -60);
            x1 = x0 + rng_rng(10, 60); y1 = y0 + rng_rng(-20, 20);
        } else {
            double side = (rng_u32() & 1) ? -1.0 : 1.0;
            x0 = side * rng_rng(220, 400); y0 = rng_rng(-20, 220);
            x1 = x0 + rng_rng(-30, 30); y1 = y0 + rng_rng(-40, 40);
        }
        segs[i].x0 = (float)x0; segs[i].y0 = (float)y0;
        segs[i].x1 = (float)x1; segs[i].y1 = (float)y1;
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (n <= 0) { fprintf(stderr, "usage: %s <segments> [out.txt]\n", argv[0]); return 2; }
    char path[256];
    if (argc > 2) snprintf(path, sizeof path, "%s", argv[2]);
    else snprintf(path, sizeof path, "bench_%d.txt", n);

    ShadowMap a, b;
    if (sh_init(&a, MAP_W, MAP_H) || sh_init(&b, MAP_W, MAP_H)) return 1;

    ShSeg *segs = (ShSeg*)malloc(sizeof(ShSeg) * (size_t)n);
    int *cand = (int*)malloc(sizeof(int) * (size_t)n);
    int *hand = (int*)malloc(sizeof(int) * (size_t)n);
    if (!segs || !cand || !hand) return 1;
    make_scene(segs, n);

    ShCullOpts on;
    memset(&on, 0, sizeof on);
    on.enabled = 1;
    ShError err;
    if (sh_frustum_from_light(&a, &LIGHT, &on.frustum, &err) != SH_OK) {
        fprintf(stderr, "frustum: %s\n", err.msg);
        return 1;
    }

    /* Hand baseline: exhaustive pixel-centre test per segment. */
    double t_hand0 = now_ms();
    int hand_kept = 0;
    for (int i = 0; i < n; ++i) {
        hand[i] = hand_blocks_any(&a, &segs[i]);
        hand_kept += hand[i];
    }
    double t_hand = now_ms() - t_hand0;

    /* Layer 1 alone. */
    ShCullStats st;
    double t_cull0 = now_ms();
    if (sh_cull_segments(&on.frustum, segs, n, cand, n, &st, &err) != SH_OK) {
        fprintf(stderr, "cull: %s (%d)\n", err.msg, err.index);
        return 1;
    }
    double t_cull = now_ms() - t_cull0;

    /* Brute force: culling disabled, every segment tested per pixel. */
    double t_brute0 = now_ms();
    ShCullOpts off = on;
    off.enabled = 0;
    if (sh_render_ex(&a, &LIGHT, segs, n, &off, NULL, &err) != SH_OK) {
        fprintf(stderr, "brute: %s (%d)\n", err.msg, err.index);
        return 1;
    }
    double t_brute = now_ms() - t_brute0;

    /* Two layers: cull once, then render candidates. */
    double t_two0 = now_ms();
    if (sh_render_ex(&b, &LIGHT, segs, n, &on, &st, &err) != SH_OK) {
        fprintf(stderr, "two-layer: %s (%d)\n", err.msg, err.index);
        return 1;
    }
    double t_two = now_ms() - t_two0;

    /* Error of the two-layer map against the brute-force map. */
    long long nbad = 0;
    float maxerr = 0.0f;
    for (int i = 0; i < MAP_W * MAP_H; ++i) {
        float d = fabsf(a.buf[i] - b.buf[i]);
        if (d > 0.0f) ++nbad;
        if (d > maxerr) maxerr = d;
    }

    /* Safety: no segment the hand pass found occluding may be culled. */
    int missed = 0;
    for (int i = 0; i < n; ++i) {
        if (!hand[i]) continue;
        int found = 0;
        for (int k = 0; k < st.candidates; ++k) if (cand[k] == i) { found = 1; break; }
        if (!found) ++missed;
    }
    int over = st.candidates - hand_kept;   /* kept but blocks nothing: expected */

    printf("segments                  %d\n", n);
    printf("frustum vertices          %d\n", on.frustum.n);
    printf("culled                    %d\n", st.culled);
    printf("candidates                %d\n", st.candidates);
    printf("hand-kept (occlude >=1px) %d\n", hand_kept);
    printf("over-kept vs hand         %d\n", over);
    printf("hand-occluders missed     %d\n", missed);
    printf("candidate share           %.1f%%\n",
           100.0 * (double)st.candidates / (double)n);
    printf("layer-1 time              %.2f ms\n", t_cull);
    printf("brute render time         %.2f ms\n", t_brute);
    printf("two-layer render time     %.2f ms\n", t_two);
    printf("hand baseline time        %.2f ms\n", t_hand);
    printf("speedup vs brute          %.2fx\n", t_brute / (t_two > 1e-9 ? t_two : 1e-9));
    printf("map cells differing       %lld of %d\n", nbad, MAP_W * MAP_H);
    printf("max abs error             %.3g\n", (double)maxerr);

    /* Candidate table. */
    printf("candidate indices         ");
    for (int k = 0; k < st.candidates && k < 24; ++k) printf("%d ", cand[k]);
    if (st.candidates > 24) printf("... (%d total)", st.candidates);
    printf("\n");
    unsigned long long digest = 1469598103934665603ULL;
    for (int k = 0; k < st.candidates; ++k) {
        digest ^= (unsigned long long)(unsigned)cand[k];
        digest *= 1099511628211ULL;
    }
    printf("candidate digest          %016llx\n", digest);
    printf("candidate table file      %s\n", path);

    /* Determinism: the same input must give the same table. */
    {
        int *again = (int*)malloc(sizeof(int) * (size_t)n);
        ShCullStats st2;
        if (!again) return 1;
        if (sh_cull_segments(&on.frustum, segs, n, again, n, &st2, NULL) != SH_OK) return 1;
        int same = st2.candidates == st.candidates &&
                   memcmp(again, cand, sizeof(int) * (size_t)st.candidates) == 0;
        printf("candidate table stable    %s\n", same ? "yes" : "NO");
        free(again);
    }

    /* Full table: one row per segment. */
    {
        FILE *f = fopen(path, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
        unsigned char *keep = (unsigned char*)calloc((size_t)n, 1);
        if (!keep) { fclose(f); return 1; }
        for (int k = 0; k < st.candidates; ++k) keep[cand[k]] = 1;
        fprintf(f, "index,x0,y0,x1,y1,candidate,blocks_pixel\n");
        for (int i = 0; i < n; ++i)
            fprintf(f, "%d,%.4f,%.4f,%.4f,%.4f,%d,%d\n",
                    i, segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1,
                    keep[i], hand[i]);
        free(keep);
        fclose(f);
    }

    free(segs); free(cand); free(hand);
    sh_free(&a); sh_free(&b);
    return (missed == 0 && nbad == 0) ? 0 : 1;
}
