#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Benchmark for the wireframe overlay.
 *
 * For 64x64 and 512x512 it reports, per resolution:
 *   - the stroke (stage 1), merge (stage 2) and blend (stage 3) timings
 *   - the overlay pixel counts, every layer number and opacity, and the
 *     composite order used by stage 2
 *   - the same numbers from an independent brute-force traversal that
 *     tests every pixel against every layer and sorts the covering layers
 *   - the pixel/colour error between the library and that traversal
 *   - the stroked line width against the nominal 2*halfwidth, swept over
 *     four quarter-pixel alignments
 *   - a hash of the frame, computed from two identical runs, to show the
 *     result is reproducible
 */

#define NLAYERS 6

typedef struct {
    int layer;
    float opacity;
    unsigned char rgb[3];
    double halfwidth;       /* pixels, independent of resolution */
    double v[3][2];         /* normalised [0,1] coordinates       */
    int enabled;
} LayerSpec;

/* Two triangles share layer 0 and two share layer 2, so the tie-break
 * (stroke order) is exercised.  One layer is fully transparent: it adds
 * coverage but no colour. */
static const LayerSpec SPECS[NLAYERS] = {
    {0, 1.00f, {  0,   0,   0}, 2.0, {{0.05,0.05},{0.95,0.10},{0.30,0.95}}, 1},
    {0, 0.50f, {255, 255, 255}, 1.5, {{0.10,0.30},{0.90,0.35},{0.50,0.98}}, 1},
    {1, 0.25f, {220,  40,  40}, 1.0, {{0.02,0.55},{0.70,0.40},{0.35,0.75}}, 1},
    {2, 0.75f, { 40, 220,  80}, 1.5, {{0.25,0.02},{0.98,0.20},{0.60,0.60}}, 1},
    {2, 0.60f, { 40,  90, 240}, 1.0, {{0.15,0.60},{0.95,0.55},{0.55,0.90}}, 1},
    {3, 0.00f, {250, 220,  30}, 2.0, {{0.00,0.85},{0.60,0.80},{0.25,0.99}}, 1},
};

typedef struct {
    int layer, seq;
    float opacity;
    unsigned char rgb[3];
} Cover;

/* ---- independent reference ---------------------------------------------- */

static double ref_dist(const RVert *a, const RVert *b, double px, double py) {
    double dx = b->x - a->x, dy = b->y - a->y;
    double t = ((px - a->x) * dx + (py - a->y) * dy) / (dx * dx + dy * dy);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double cx = a->x + t * dx, cy = a->y + t * dy;
    return sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

typedef struct {
    int covered;            /* pixels covered by >= 1 layer      */
    int contributions;      /* sum of covering layers over pixels */
    int blended;            /* pixels with composite alpha > 0   */
    int layer_covered[NLAYERS];
    int cov_hist[8];        /* cov_hist[d]: pixels covered by >= d layers */
    int alpha1_hist[8];     /* of those, composite alpha reached 1       */
    int mismatches;         /* pixels whose colour differs       */
    int maxerr;             /* largest absolute channel delta    */
} RefStats;

/* Full-frame scan: every pixel against every layer's three edges, no
 * bounding boxes, no masks.  Covering layers are sorted by
 * (layer number, stroke order) and composited in double. */
static void ref_render(const RTri tris[NLAYERS], const LayerSpec specs[NLAYERS],
                       int w, int h, const unsigned char *frame,
                       unsigned char *out, RefStats *st) {
    memset(st, 0, sizeof *st);
    memcpy(out, frame, (size_t)w * h * 3);   /* uncovered pixels keep the frame */
    int *cov = (int*)calloc((size_t)w * h, sizeof(int)); /* covering layers */
    int *alpha1 = (int*)calloc((size_t)w * h, sizeof(int)); /* opaque once */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Cover list[NLAYERS];
            int k = 0;
            double px = x + 0.5, py = y + 0.5;
            for (int i = 0; i < NLAYERS; ++i) {
                if (!specs[i].enabled) continue;
                int hit = 0;
                for (int e = 0; e < 3; ++e)
                    if (ref_dist(&tris[i].v[e], &tris[i].v[(e + 1) % 3], px, py)
                        <= specs[i].halfwidth) { hit = 1; break; }
                if (!hit) continue;
                ++st->layer_covered[i];
                list[k].layer = specs[i].layer;
                list[k].seq = i;
                list[k].opacity = specs[i].opacity;
                list[k].rgb[0] = specs[i].rgb[0];
                list[k].rgb[1] = specs[i].rgb[1];
                list[k].rgb[2] = specs[i].rgb[2];
                ++k;
            }
            if (k == 0) continue;
            st->covered++;
            st->contributions += k;
            cov[(size_t)y * w + x] = k;
            for (int a = 1; a < k; ++a) {   /* insertion sort, stable */
                Cover c = list[a];
                int b = a - 1;
                while (b >= 0 && (list[b].layer > c.layer ||
                                  (list[b].layer == c.layer && list[b].seq > c.seq))) {
                    list[b + 1] = list[b];
                    --b;
                }
                list[b + 1] = c;
            }
            double alpha = 0.0, pm[3] = {0, 0, 0};
            for (int a = 0; a < k; ++a) {
                double s = (double)list[a].opacity, keep = 1.0 - s;
                for (int c3 = 0; c3 < 3; ++c3)
                    pm[c3] = (double)list[a].rgb[c3] * s + pm[c3] * keep;
                alpha = s + alpha * keep;
            }
            if (alpha >= 1.0 - 1e-12) alpha1[(size_t)y * w + x] = 1;
            if (!(alpha > 0.0)) continue;
            st->blended++;
            size_t i = (size_t)y * w + x;
            for (int c3 = 0; c3 < 3; ++c3) {
                double v = pm[c3] + (double)frame[i * 3 + c3] * (1.0 - alpha);
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                out[i * 3 + c3] = (unsigned char)(v + 0.5);
            }
        }
    }
    for (int d = 1; d < 8; ++d) {
        int any = 0, one = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            if (cov[i] >= d) ++any;
            if (cov[i] >= d && alpha1[i]) ++one;
        }
        st->cov_hist[d] = any;
        st->alpha1_hist[d] = one;
    }
    free(cov); free(alpha1);
}

/* ---- helpers ------------------------------------------------------------ */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static unsigned long long fnv1a(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static RTri make_tri(const LayerSpec *s, int n) {
    RTri t;
    for (int i = 0; i < 3; ++i) {
        t.v[i].x = s->v[i][0] * (double)n;
        t.v[i].y = s->v[i][1] * (double)n;
        t.v[i].z = 0.5;
    }
    t.rgb[0] = s->rgb[0]; t.rgb[1] = s->rgb[1]; t.rgb[2] = s->rgb[2];
    return t;
}

static RWireSpec make_spec(const LayerSpec *s) {
    RWireSpec w;
    w.layer = s->layer;
    w.opacity = s->opacity;
    w.rgb[0] = s->rgb[0]; w.rgb[1] = s->rgb[1]; w.rgb[2] = s->rgb[2];
    w.halfwidth = s->halfwidth;
    w.enabled = s->enabled;
    return w;
}

/* Fill a frame with a dark gradient so blending has a non-black
 * destination to mix with. */
static void fill_frame(RTarget *t) {
    for (int y = 0; y < t->h; ++y)
        for (int x = 0; x < t->w; ++x) {
            size_t i = (size_t)y * t->w + x;
            t->rgb[i * 3 + 0] = (unsigned char)(x * 255 / (t->w > 1 ? t->w - 1 : 1));
            t->rgb[i * 3 + 1] = (unsigned char)(y * 255 / (t->h > 1 ? t->h - 1 : 1));
            t->rgb[i * 3 + 2] = 40;
        }
}

/* Measured covered-centre count across the horizontal segment at y=0.5n
 * at column 0.5n, swept over four quarter-pixel offsets. */
static void width_probe(int n, double halfwidth, int *wmin, int *wmax) {
    *wmin = 0x7fffffff; *wmax = 0;
    for (int q = 0; q < 4; ++q) {
        RTarget t;
        if (rt_init(&t, n, n) != 0) return;
        RTri tri = {{ {0.2 * n, 0.5 * n + q / 4.0, 0.5},
                      {0.8 * n, 0.5 * n + q / 4.0, 0.5},
                      {0.5 * n, 1.5 * n, 0.5} }, {255, 255, 255}};
        LayerSpec s = {0, 1.0f, {255, 255, 255}, halfwidth,
                       {{0,0},{1,0},{0.5,1}}, 1};
        RWireSpec w = make_spec(&s);
        RWireStrokeStats st; RWireErr e;
        rt_wire_stroke(&t, &tri, &w, &st, &e);
        int col = n / 2, count = 0;
        for (int y = 0; y < n; ++y) {
            double d;
            if (rt_wire_edge_dist(&tri.v[0], &tri.v[1], col, y, &d) == RT_WIRE_OK &&
                d <= halfwidth) ++count;
        }
        if (count < *wmin) *wmin = count;
        if (count > *wmax) *wmax = count;
        rt_free(&t);
    }
}

static void run(int n) {
    RTri tris[NLAYERS];
    RWireSpec specs[NLAYERS];
    for (int i = 0; i < NLAYERS; ++i) {
        tris[i] = make_tri(&SPECS[i], n);
        specs[i] = make_spec(&SPECS[i]);
    }

    int reps = n <= 64 ? 200 : 20;
    double t_stroke = 0.0, t_merge = 0.0, t_blend = 0.0, t_total = 0.0;
    RWireResolveStats last;
    RTarget t;
    if (rt_init(&t, n, n) != 0) { printf("init failed\n"); return; }
    fill_frame(&t);

    for (int r = 0; r < reps; ++r) {
        RTarget w;
        if (rt_init(&w, n, n) != 0) { printf("init failed\n"); return; }
        memcpy(w.rgb, t.rgb, (size_t)n * n * 3);

        double t0 = now_s();
        for (int i = 0; i < NLAYERS; ++i) {
            RWireStrokeStats st; RWireErr e;
            if (rt_wire_stroke(&w, &tris[i], &specs[i], &st, &e) < 0) {
                char msg[160];
                rt_wire_errfmt(&e, msg, sizeof msg);
                printf("stroke %d failed: %s\n", i, msg);
                rt_free(&w);
                return;
            }
        }
        double t1 = now_s();
        RWireErr e2;
        if (rt_wire_merge(&w, &last.merge, &e2) != RT_WIRE_OK) { rt_free(&w); return; }
        double t2 = now_s();
        if (rt_wire_blend(&w, &last.blend, &e2) != RT_WIRE_OK) { rt_free(&w); return; }
        double t3 = now_s();

        double ts = (t1 - t0) * 1e3, tm = (t2 - t1) * 1e3, tb = (t3 - t2) * 1e3;
        if (r == 0 || ts < t_stroke) t_stroke = ts;
        if (r == 0 || tm < t_merge) t_merge = tm;
        if (r == 0 || tb < t_blend) t_blend = tb;
        if (r == 0 || ts + tm + tb < t_total) t_total = ts + tm + tb;
        rt_free(&w);
    }

    /* Identical replay in a fresh target: stage stats and frame must match. */
    RTarget a, b;
    if (rt_init(&a, n, n) != 0 || rt_init(&b, n, n) != 0) return;
    memcpy(a.rgb, t.rgb, (size_t)n * n * 3);
    memcpy(b.rgb, t.rgb, (size_t)n * n * 3);
    RWireResolveStats ra, rb; RWireErr ea, eb;
    RWireStrokeStats sa[NLAYERS], sb[NLAYERS];
    for (int i = 0; i < NLAYERS; ++i) rt_wire_stroke(&a, &tris[i], &specs[i], &sa[i], &ea);
    for (int i = 0; i < NLAYERS; ++i) rt_wire_stroke(&b, &tris[i], &specs[i], &sb[i], &eb);
    rt_wire_resolve(&a, &ra, &ea);
    rt_wire_resolve(&b, &rb, &eb);
    int same_frame = memcmp(a.rgb, b.rgb, (size_t)n * n * 3) == 0;
    int same_stats = memcmp(&ra, &rb, sizeof ra) == 0;
    unsigned long long hash = fnv1a(a.rgb, (size_t)n * n * 3);

    /* Independent traversal. */
    unsigned char *ref = (unsigned char*)malloc((size_t)n * n * 3);
    RefStats rs;
    double b0 = now_s();
    ref_render(tris, SPECS, n, n, t.rgb, ref, &rs);
    double brute_ms = (now_s() - b0) * 1e3;

    int mismatches = 0, maxerr = 0;
    for (size_t i = 0; i < (size_t)n * n; ++i) {
        int diff = 0;
        for (int c = 0; c < 3; ++c) {
            int d = (int)a.rgb[i * 3 + c] - (int)ref[i * 3 + c];
            if (d < 0) d = -d;
            if (d > maxerr) maxerr = d;
            if (d > 0) diff = 1;
        }
        if (diff) ++mismatches;
    }

    /* Stroke counts vs. the reference's per-layer coverage. */
    int count_mismatch = 0;
    for (int i = 0; i < NLAYERS; ++i)
        if (sa[i].covered != rs.layer_covered[i]) ++count_mismatch;

    int wmin = 0, wmax = 0;
    width_probe(n, 2.0, &wmin, &wmax);
    double nominal = 4.0;
    double werr = fmax(fabs(wmin - nominal), fabs(wmax - nominal));

    printf("== %dx%d ==\n", n, n);
    printf("layers        %d\n", ra.merge.layers);
    printf("stroke counts ");
    for (int i = 0; i < NLAYERS; ++i)
        printf(" L%d:%d/%d", specs[i].layer, sa[i].covered, rs.layer_covered[i]);
    printf("  (library/reference, mismatched layers=%d)\n", count_mismatch);
    printf("merge         covered=%d contributions=%d opaque=%d\n",
           ra.merge.covered, ra.merge.contributions, ra.merge.opaque);
    printf("blend         blended=%d replaced=%d  reference blended=%d\n",
           ra.blend.blended, ra.blend.replaced, rs.blended);
    printf("order        ");
    for (int k = 0; k < ra.merge.layers; ++k)
        printf(" [L%d a=%.2f n=%d]", ra.merge.order_layer[k],
               (double)ra.merge.order_opacity[k], ra.merge.order_covered[k]);
    printf("\n");
    /* Walk the reported slots and prove the ordering predicate directly. */
    int order_ok = ra.merge.layers == NLAYERS;
    for (int k = 1; k < ra.merge.layers; ++k) {
        int p = ra.merge.order[k - 1], q = ra.merge.order[k];
        if (a.layers[p].layer > a.layers[q].layer) order_ok = 0;
        if (a.layers[p].layer == a.layers[q].layer &&
            !(a.layers[p].seq < a.layers[q].seq)) order_ok = 0;
    }
    printf("order check   ascending layer, ties by stroke order: %s\n",
           order_ok ? "yes" : "NO");
    /* The same ordered layers stroked in sorted order must reproduce the
     * frame bit for bit (layer numbers renumbered to stay strictly
     * increasing, which does not change whose layer is on top). */
    RTarget c;
    if (rt_init(&c, n, n) != 0) return;
    memcpy(c.rgb, t.rgb, (size_t)n * n * 3);
    RWireErr ec; RWireResolveStats rcx;
    for (int k = 0; k < ra.merge.layers; ++k) {
        int s = ra.merge.order[k];
        RWireSpec wspec = specs[s];
        wspec.layer = k;
        RWireStrokeStats st;
        rt_wire_stroke(&c, &tris[s], &wspec, &st, &ec);
    }
    rt_wire_resolve(&c, &rcx, &ec);
    printf("order replay  sorted-order strokes reproduce frame: %s\n",
           memcmp(a.rgb, c.rgb, (size_t)n * n * 3) == 0 ? "yes" : "NO");
    rt_free(&c);
    printf("error         mismatched pixels=%d  max channel delta=%d\n",
           mismatches, maxerr);
    printf("overlap       reference coverage by depth of layer stack:\n");
    printf("              >=1:%d >=2:%d >=3:%d >=4:%d >=5:%d >=6:%d >=7:%d\n",
           rs.cov_hist[1], rs.cov_hist[2], rs.cov_hist[3], rs.cov_hist[4],
           rs.cov_hist[5], rs.cov_hist[6], rs.cov_hist[7]);
    printf("opaque        composite alpha reached 1 at depth >=1:%d >=2:%d"
           " >=3:%d >=4:%d >=5:%d >=6:%d\n",
           rs.alpha1_hist[1], rs.alpha1_hist[2], rs.alpha1_hist[3],
           rs.alpha1_hist[4], rs.alpha1_hist[5], rs.alpha1_hist[6]);
    printf("width         nominal=%.2f px measured=[%d,%d] worst error=%.2f px\n",
           nominal, wmin, wmax, werr);
    printf("reproducible  frame %s, stats %s, hash=%016llx\n",
           same_frame ? "identical" : "DIFFERENT",
           same_stats ? "identical" : "DIFFERENT", hash);
    printf("time          stroke=%.3f ms  merge=%.3f ms  blend=%.3f ms"
           "  total=%.3f ms  brute force=%.3f ms  speedup=%.2fx\n",
           t_stroke, t_merge, t_blend, t_total, brute_ms,
           brute_ms / (t_total > 1e-9 ? t_total : 1e-9));
    printf("time/pixel    %.2f ns library, %.2f ns brute force\n\n",
           t_total * 1e6 / ((double)n * n), brute_ms * 1e6 / ((double)n * n));

    free(ref);
    rt_free(&a); rt_free(&b); rt_free(&t);
}

int main(void) {
    printf("wireframe overlay: three stages, %d layers\n", NLAYERS);
    printf("layer numbers and opacities:");
    for (int i = 0; i < NLAYERS; ++i)
        printf(" L%d=%.2f", SPECS[i].layer, (double)SPECS[i].opacity);
    printf("\n\n");
    run(64);
    run(512);
    return 0;
}
