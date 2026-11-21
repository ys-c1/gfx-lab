#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- fixtures ------------------------------------------------------------
 * Right-side-up triangle, 32x32, halfwidth 0.5.  Every count below was
 * worked out by hand from the pixel-centre distance predicate and is
 * asserted exactly, so any drift in distance or thresholding shows up.
 *   v0=(4.5,4.5) v1=(28.5,4.5) v2=(16.5,28.5)
 *   per-edge [25, 37, 37], union 95; overlaps: (4,4) [e0,e2],
 *   (28,4) [e0,e1], (16,27),(16,28) [e1,e2].
 */
static const RTri TRI32 = {{ {4.5,4.5,0.5}, {28.5,4.5,0.5}, {16.5,28.5,0.5} }, {255, 0, 0}};
/* The same shape scaled 4x into 64x64 with halfwidth 1.0:
 *   v0=(8,8) v1=(56,8) v2=(32,56): per-edge [100,100,100], union 288. */
static const RTri TRI64 = {{ {8,8,0.5}, {56,8,0.5}, {32,56,0.5} }, {255, 0, 0}};

static RWireSpec spec(int layer, float opacity, double hw, unsigned char r,
                      unsigned char g, unsigned char b, int enabled) {
    RWireSpec s;
    s.layer = layer; s.opacity = opacity;
    s.rgb[0] = r; s.rgb[1] = g; s.rgb[2] = b;
    s.halfwidth = hw; s.enabled = enabled;
    return s;
}

static int snapshot(RTarget *t, float **z, unsigned char **rgb) {
    size_t n = (size_t)t->w * (size_t)t->h;
    *z = (float*)malloc(sizeof(float) * n);
    *rgb = (unsigned char*)malloc(n * 3);
    if (!*z || !*rgb) return 0;
    memcpy(*z, t->z, sizeof(float) * n);
    memcpy(*rgb, t->rgb, n * 3);
    return 1;
}

static int unchanged(const RTarget *t, const float *z, const unsigned char *rgb) {
    size_t n = (size_t)t->w * (size_t)t->h;
    return memcmp(z, t->z, sizeof(float) * n) == 0 &&
           memcmp(rgb, t->rgb, n * 3) == 0;
}

/* ---- 1. stage 1: edge-function distance -------------------------------- */
static void test_edge_dist(void) {
    RVert a = {4.5, 4.5, 0.0}, b = {28.5, 4.5, 0.0};
    double d = -1.0;
    /* centres are at integer + 0.5; the horizontal edge is at y=4.5 */
    CK(rt_wire_edge_dist(&a, &b, 16, 4, &d) == RT_WIRE_OK && fabs(d - 0.0) < 1e-12);
    CK(rt_wire_edge_dist(&a, &b, 16, 5, &d) == RT_WIRE_OK && fabs(d - 1.0) < 1e-12);
    CK(rt_wire_edge_dist(&a, &b, 16, 2, &d) == RT_WIRE_OK && fabs(d - 2.0) < 1e-12);
    /* past the endpoint the cap applies: centre (30.5,4.5) is 2.0 from b,
     * not 0 as an infinite line would report */
    CK(rt_wire_edge_dist(&a, &b, 30, 4, &d) == RT_WIRE_OK && fabs(d - 2.0) < 1e-12);
    /* 45-degree edge: perpendicular distance of a centre on the diagonal
     * is 0, one pixel off it is 1/sqrt(2). */
    RVert p = {0.0, 0.0, 0.0}, q = {10.0, 10.0, 0.0};
    CK(rt_wire_edge_dist(&p, &q, 4, 4, &d) == RT_WIRE_OK && fabs(d) < 1e-12);
    CK(rt_wire_edge_dist(&p, &q, 4, 5, &d) == RT_WIRE_OK &&
       fabs(d - 1.0 / sqrt(2.0)) < 1e-12);
    /* degenerate segment is refused before the division */
    RVert same = {7.0, 7.0, 0.0};
    CK(rt_wire_edge_dist(&same, &same, 0, 0, &d) == RT_WIRE_ERR_DUP_VERTEX);
    CK(rt_wire_edge_dist(NULL, &b, 0, 0, &d) == RT_WIRE_ERR_ARG);
}

/* ---- 2. stage 1: stroke writes only its own mask ----------------------- */
static void test_stroke(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    /* A fill first: the overlay must live on top of / beside it, not in it. */
    RTri fill = {{ {2,2,0.9}, {30,2,0.9}, {16,30,0.9} }, {10, 200, 30}};
    CK(rt_triangle(&t, &fill) > 0);
    float *z0; unsigned char *rgb0;
    CK(snapshot(&t, &z0, &rgb0));

    RWireSpec w = spec(0, 0.5f, 0.5, 255, 255, 255, 1);
    RWireStrokeStats s; RWireErr e;
    CK(rt_wire_stroke(&t, &TRI32, &w, &s, &e) == 95);
    CK(s.per_edge[0] == 25 && s.per_edge[1] == 37 && s.per_edge[2] == 37);
    CK(s.covered == 95);
    CK(s.layer_slot == 0 && s.layer == 0 && s.opacity == 0.5f);
    /* Stroke alone must leave the frame and depth untouched. */
    CK(unchanged(&t, z0, rgb0));

    /* A second stroke with a different layer is a separate entry. */
    RWireSpec w2 = spec(2, 1.0f, 0.0, 0, 0, 255, 1);
    CK(rt_wire_stroke(&t, &TRI64, &w2, &s, &e) >= 0);
    CK(s.layer_slot == 1 && s.layer == 2);

    /* Half width 0 draws only centres exactly on an edge. */
    RTarget z;
    CK(rt_init(&z, 32, 32) == 0);
    RWireSpec w0 = spec(0, 1.0f, 0.0, 9, 9, 9, 1);
    CK(rt_wire_stroke(&z, &TRI32, &w0, &s, &e) > 0 && s.per_edge[0] > 0);
    /* Disabled spec: no validation, no allocation, no change. */
    RWireSpec off = spec(-5, -3.0f, -1.0, 1, 2, 3, 0);
    RTri degenerate = {{ {1,1,0}, {1,1,0}, {9,9,0} }, {0, 0, 0}};
    CK(rt_wire_stroke(&z, &degenerate, &off, &s, &e) == RT_WIRE_OK);
    CK(s.covered == 0 && s.layer_slot == -1 && z.layer_count == 1);
    /* Huge half width clamps to the target instead of overflowing. */
    RWireSpec huge = spec(0, 1.0f, 1e9, 255, 255, 255, 1);
    CK(rt_wire_stroke(&z, &TRI32, &huge, &s, &e) == 32 * 32);
    rt_free(&z);
    rt_free(&t);
    free(z0); free(rgb0);
}

/* ---- 3. stage 2/3: independent compositing ------------------------------ */
static void test_blend_values(void) {
    /* Triangle far from the origin so the mask spans known pixels; the
     * frame starts black so dst = (0,0,0) there except where filled. */
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RWireSpec a = spec(1, 0.25f, 0.5, 250, 100, 50, 1);
    RWireSpec b = spec(1, 0.5f, 0.5, 0, 255, 0, 1);
    RWireStrokeStats ss; RWireErr e;
    CK(rt_wire_stroke(&t, &TRI32, &a, &ss, &e) == 95);
    CK(rt_wire_stroke(&t, &TRI32, &b, &ss, &e) == 95);
    RWireResolveStats r;
    CK(rt_wire_merge(&t, &r.merge, &e) == RT_WIRE_OK);
    /* Same layer number: stroke order is the tie-breaker, so a then b. */
    CK(r.merge.layers == 2);
    CK(r.merge.order_layer[0] == 1 && r.merge.order_layer[1] == 1);
    CK(r.merge.order[0] == 0 && r.merge.order[1] == 1);
    /* Merge does not write the frame. */
    for (int i = 0; i < 32 * 32 * 3; ++i) CK(t.rgb[i] == 0);
    CK(rt_wire_blend(&t, &r.blend, &e) == RT_WIRE_OK);
    CK(r.blend.blended == 95 && r.blend.replaced == 0);
    /* Composite over black: a=0.25 then b=0.5 ->
     *   alpha 0.625, colour (250*.25*(1-.5), ... ) premultiplied. */
    int px = 4 * 32 + 16; /* (16,4): on the horizontal edge, inside the union */
    /* value = pm + 0*(1-a) = pm */
    /* pm_r = 250*0.25 + 0*0.75*... computed layer by layer:
     *   after a: pm=(62.5,25,12.5), A=.25
     *   after b: pm=(0*0.5+62.5*0.5, 255*0.5+25*0.5, 0*0.5+12.5*0.5)
     *           = (31.25, 140, 6.25), A=.25+.5*.75=.625
     */
    CK(t.rgb[px * 3 + 0] == 31);
    CK(t.rgb[px * 3 + 1] == 140);
    CK(t.rgb[px * 3 + 2] == 6);
    /* Depth was never touched. */
    CK(t.z[px] > 1e29f);
    /* A second blend call finds nothing staged: it is consumed once. */
    CK(rt_wire_blend(&t, &r.blend, &e) == RT_WIRE_OK && r.blend.blended == 0);
    rt_free(&t);

    /* Layer order beats stroke order. */
    RTarget u;
    CK(rt_init(&u, 32, 32) == 0);
    RWireSpec hi = spec(5, 0.0f, 0.5, 1, 2, 3, 1);   /* opaque-ish later */
    RWireSpec lo = spec(1, 1.0f, 0.5, 200, 0, 0, 1); /* layer 1, full */
    RWireSpec hi2 = spec(9, 1.0f, 0.5, 0, 0, 200, 1);
    CK(rt_wire_stroke(&u, &TRI32, &hi, &ss, &e) == 95);  /* layer 5, a=0 */
    CK(rt_wire_stroke(&u, &TRI32, &lo, &ss, &e) == 95);  /* layer 1 */
    CK(rt_wire_stroke(&u, &TRI32, &hi2, &ss, &e) == 95); /* layer 9 */
    RWireResolveStats ru;
    CK(rt_wire_resolve(&u, &ru, &e) == RT_WIRE_OK);
    CK(ru.merge.order_layer[0] == 1 && ru.merge.order_layer[1] == 5 &&
       ru.merge.order_layer[2] == 9);
    /* layer 9 is opaque and last, so it wins everywhere it covers. */
    CK(u.rgb[(4 * 32 + 16) * 3 + 2] == 200);
    CK(u.rgb[(4 * 32 + 16) * 3 + 0] == 0);
    rt_free(&u);
}

/* ---- 4. disabled is a complete no-op ----------------------------------- */
static void test_disabled_noop(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri fill = {{ {2,2,0.9}, {30,2,0.9}, {16,30,0.9} }, {10, 200, 30}};
    CK(rt_triangle(&t, &fill) > 0);
    float *z0; unsigned char *rgb0;
    CK(snapshot(&t, &z0, &rgb0));

    RWireSpec off = spec(0, 0.5f, 0.5, 255, 255, 255, 0);
    RWireStrokeStats ss; RWireErr e;
    CK(rt_wire_stroke(&t, &TRI32, &off, &ss, &e) == RT_WIRE_OK);
    RWireResolveStats r;
    CK(rt_wire_resolve(&t, &r, &e) == RT_WIRE_OK);
    CK(r.merge.layers == 0 && r.blend.blended == 0);
    CK(unchanged(&t, z0, rgb0));

    /* A stroke that is then abandoned (clear) also leaves the frame alone. */
    RWireSpec on = spec(0, 1.0f, 0.5, 255, 255, 255, 1);
    CK(rt_wire_stroke(&t, &TRI32, &on, &ss, &e) == 95);
    rt_wire_clear(&t);
    CK(unchanged(&t, z0, rgb0));
    /* Clear is idempotent and safe on a freshly initialised target. */
    rt_wire_clear(&t);
    CK(t.layer_count == 0);
    rt_free(&t);
    free(z0); free(rgb0);

    /* Blank target, no layers: resolve writes nothing. */
    RTarget v;
    CK(rt_init(&v, 8, 8) == 0);
    float *z1; unsigned char *rgb1;
    CK(snapshot(&v, &z1, &rgb1));
    CK(rt_wire_resolve(&v, &r, &e) == RT_WIRE_OK);
    CK(unchanged(&v, z1, rgb1));
    rt_free(&v);
    free(z1); free(rgb1);
}

/* ---- 5. rejected values and their locations ---------------------------- */
static void test_errors(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RWireStrokeStats s; RWireErr e;
    char msg[160];

    RWireSpec ok = spec(3, 0.5f, 0.5, 0, 255, 0, 1);
    CK(rt_wire_stroke(&t, &TRI32, &ok, &s, &e) == 95);
    /* Every rejection below must not add a layer. */
    int before = t.layer_count;

    /* Negative layer: reported with the layer number and field name. */
    RWireSpec badl = ok; badl.layer = -1;
    CK(rt_wire_stroke(&t, &TRI32, &badl, &s, &e) == RT_WIRE_ERR_LAYER);
    CK(e.code == RT_WIRE_ERR_LAYER && e.layer == -1);
    CK(strcmp(e.field, "layer") == 0);
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0 && strstr(msg, "layer") != NULL);

    /* Opacity out of range, including NaN. */
    RWireSpec badp = ok; badp.opacity = 1.5f;
    CK(rt_wire_stroke(&t, &TRI32, &badp, &s, &e) == RT_WIRE_ERR_OPACITY);
    CK(e.layer == 3 && strcmp(e.field, "opacity") == 0);
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0 &&
       strstr(msg, "layer 3") != NULL && strstr(msg, "opacity") != NULL);
    badp.opacity = -0.1f;
    CK(rt_wire_stroke(&t, &TRI32, &badp, &s, &e) == RT_WIRE_ERR_OPACITY);
    badp.opacity = NAN;
    CK(rt_wire_stroke(&t, &TRI32, &badp, &s, &e) == RT_WIRE_ERR_OPACITY);

    /* Half width negative / non-finite. */
    RWireSpec badh = ok; badh.halfwidth = -0.5;
    CK(rt_wire_stroke(&t, &TRI32, &badh, &s, &e) == RT_WIRE_ERR_HALFWIDTH);
    CK(strcmp(e.field, "halfwidth") == 0);
    badh.halfwidth = INFINITY;
    CK(rt_wire_stroke(&t, &TRI32, &badh, &s, &e) == RT_WIRE_ERR_HALFWIDTH);

    /* Non-finite vertex: names the vertex index. */
    RTri nan_tri = TRI32; nan_tri.v[2].y = NAN;
    CK(rt_wire_stroke(&t, &nan_tri, &ok, &s, &e) == RT_WIRE_ERR_VERTEX);
    CK(e.vertex == 2 && strcmp(e.field, "vertex") == 0);
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0 && strstr(msg, "vertex") != NULL);

    /* Coincident vertices: edge i is v[i] -> v[(i+1)%3], all three pairs. */
    for (int i = 0; i < 3; ++i) {
        RTri d = TRI32;
        d.v[i] = d.v[(i + 1) % 3];
        CK(rt_wire_stroke(&t, &d, &ok, &s, &e) == RT_WIRE_ERR_DUP_VERTEX);
        CK(e.edge == i && e.vertex == i && e.vertex2 == (i + 1) % 3);
        CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0);
        char want[64];
        snprintf(want, sizeof want, "edge %d (v%d-v%d)", i, i, (i + 1) % 3);
        CK(strstr(msg, want) != NULL);
    }

    /* Short edge just below RT_WIRE_MIN_EDGE on edge 2 (v2-v0). */
    RTri sh = TRI32;
    sh.v[2].x = sh.v[0].x;
    sh.v[2].y = sh.v[0].y + 1e-8;
    CK(rt_wire_stroke(&t, &sh, &ok, &s, &e) == RT_WIRE_ERR_SHORT_EDGE);
    CK(e.edge == 2 && e.vertex == 2 && e.vertex2 == 0);
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0 && strstr(msg, "v2-v0") != NULL);

    /* Nothing above created a layer or touched the frame. */
    CK(t.layer_count == before);
    CK(t.rgb[(16 * 32 + 4) * 3] == 0);

    /* NULL arguments. */
    CK(rt_wire_stroke(NULL, &TRI32, &ok, &s, &e) == RT_WIRE_ERR_ARG);
    CK(rt_wire_stroke(&t, NULL, &ok, &s, &e) == RT_WIRE_ERR_ARG);
    CK(rt_wire_stroke(&t, &TRI32, NULL, &s, &e) == RT_WIRE_ERR_ARG);

    /* Capacity: fill every layer slot, then the next stroke is refused
     * with no partial state. */
    rt_wire_clear(&t);
    for (int i = 0; i < RT_WIRE_MAX_LAYERS; ++i) {
        RWireSpec w = spec(i, 0.5f, 0.5, 255, 255, 255, 1);
        CK(rt_wire_stroke(&t, &TRI32, &w, &s, &e) == 95);
    }
    CK(t.layer_count == RT_WIRE_MAX_LAYERS);
    RWireSpec over = spec(0, 0.5f, 0.5, 255, 255, 255, 1);
    CK(rt_wire_stroke(&t, &TRI32, &over, &s, &e) == RT_WIRE_ERR_CAPACITY);
    CK(strcmp(e.field, "layers") == 0);
    rt_free(&t);
}

/* ---- 6. the three stages are genuinely separable ----------------------- */
static void test_stage_separation(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RWireSpec w = spec(0, 0.5f, 0.5, 200, 100, 50, 1);
    RWireStrokeStats ss; RWireErr e;
    RWireResolveStats r;
    CK(rt_wire_stroke(&t, &TRI32, &w, &ss, &e) == 95);
    CK(rt_wire_merge(&t, &r.merge, &e) == RT_WIRE_OK);
    /* Merge is repeatable: the second call reports the same numbers. */
    RWireMergeStats m2;
    CK(rt_wire_merge(&t, &m2, &e) == RT_WIRE_OK);
    CK(m2.covered == r.merge.covered && m2.contributions == r.merge.contributions);
    CK(m2.order[0] == 0 && m2.order_layer[0] == 0);
    CK(m2.order_opacity[0] == 0.5f);
    CK(rt_wire_blend(&t, &r.blend, &e) == RT_WIRE_OK);
    CK(r.blend.blended == 95);
    /* Over the original black frame: value = 200*0.5 = 100. */
    CK(t.rgb[(4 * 32 + 16) * 3 + 0] == 100);
    CK(t.rgb[(4 * 32 + 16) * 3 + 1] == 50);
    CK(t.rgb[(4 * 32 + 16) * 3 + 2] == 25);
    /* Stroking after a resolve invalidates the old merge: a bare blend
     * does nothing until the new state is merged. */
    CK(rt_wire_stroke(&t, &TRI32, &w, &ss, &e) == 95);
    RWireBlendStats b2;
    CK(rt_wire_blend(&t, &b2, &e) == RT_WIRE_OK && b2.blended == 0);
    rt_free(&t);
}

/* ---- 7. reproducibility: same strokes, same image, any grouping --------- */
static int render_layers(RTarget *t, const RTri *tris, const RWireSpec *specs,
                         int n, RWireResolveStats *r, RWireErr *e) {
    RWireStrokeStats s;
    for (int i = 0; i < n; ++i)
        if (rt_wire_stroke(t, &tris[i], &specs[i], &s, e) < 0) return -1;
    return rt_wire_resolve(t, r, e);
}

static void test_reproducible(void) {
    enum { N = 6 };
    RTri tris[N] = {TRI32, TRI64, TRI32, TRI64, TRI32, TRI64};
    RWireSpec sp[N] = {
        spec(2, 0.50f, 0.5, 200, 0, 0, 1),
        spec(0, 1.00f, 1.0, 0, 200, 0, 1),
        spec(1, 0.25f, 1.5, 0, 0, 200, 1),
        spec(2, 0.75f, 0.5, 200, 200, 0, 1),
        spec(1, 0.60f, 2.0, 0, 200, 200, 1),
        spec(0, 0.20f, 1.0, 200, 0, 200, 1),
    };
    RTarget a, b;
    CK(rt_init(&a, 64, 64) == 0);
    CK(rt_init(&b, 64, 64) == 0);
    RWireResolveStats ra, rb; RWireErr ea, eb;
    CK(render_layers(&a, tris, sp, N, &ra, &ea) == RT_WIRE_OK);
    CK(render_layers(&b, tris, sp, N, &rb, &eb) == RT_WIRE_OK);
    CK(memcmp(a.rgb, b.rgb, (size_t)64 * 64 * 3) == 0);
    CK(ra.merge.layers == N && ra.blend.blended == rb.blend.blended);
    /* The reported order is ascending layer, ties by stroke order. */
    for (int k = 1; k < N; ++k) {
        int p = ra.merge.order[k - 1], q = ra.merge.order[k];
        CK(a.layers[p].layer <= a.layers[q].layer);
        if (a.layers[p].layer == a.layers[q].layer)
            CK(a.layers[p].seq < a.layers[q].seq);
        CK(ra.merge.order_layer[k] == a.layers[q].layer);
        CK(ra.merge.order_opacity[k] == a.layers[q].opacity);
        CK(ra.merge.order_covered[k] == a.layers[q].covered);
    }
    /* layers=0 first vs. stroked later: (layer, seq) ordering means a
     * layer-0 stroke made later still composites first.  Render the
     * same ordered list in the sorted sequence and compare. */
    RTarget c;
    CK(rt_init(&c, 64, 64) == 0);
    RTri ct[N]; RWireSpec cs[N];
    for (int k = 0; k < N; ++k) {
        int s = ra.merge.order[k];
        ct[k] = tris[s]; cs[k] = sp[s];
        cs[k].layer = k; /* strictly increasing, same relative order */
    }
    RWireResolveStats rc; RWireErr ec;
    CK(render_layers(&c, ct, cs, N, &rc, &ec) == RT_WIRE_OK);
    CK(memcmp(a.rgb, c.rgb, (size_t)64 * 64 * 3) == 0);
    rt_free(&a); rt_free(&b); rt_free(&c);
}

/* ---- 8. independent brute-force reference ------------------------------ */
/* No library code: for each pixel, collect covering layers with a plain
 * quadratic segment distance, sort by (layer, stroke order), composite
 * into doubles, and round once at the end. */
typedef struct { int layer, seq; float opacity; unsigned char rgb[3]; } RefLayer;

static double ref_seg_dist(const RVert *a, const RVert *b, double px, double py) {
    double dx = b->x - a->x, dy = b->y - a->y;
    double t = ((px - a->x) * dx + (py - a->y) * dy) / (dx * dx + dy * dy);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double cx = a->x + t * dx, cy = a->y + t * dy;
    return sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

static int ref_covering(const RTri *tri, const RWireSpec *sp, int seq,
                        double px, double py, RefLayer *out) {
    for (int i = 0; i < 3; ++i)
        if (ref_seg_dist(&tri->v[i], &tri->v[(i + 1) % 3], px, py) <= sp->halfwidth) {
            out->layer = sp->layer; out->seq = seq; out->opacity = sp->opacity;
            out->rgb[0] = sp->rgb[0]; out->rgb[1] = sp->rgb[1]; out->rgb[2] = sp->rgb[2];
            return 1;
        }
    return 0;
}

static int ref_cmp(const void *pa, const void *pb) {
    const RefLayer *a = (const RefLayer*)pa, *b = (const RefLayer*)pb;
    if (a->layer != b->layer) return a->layer < b->layer ? -1 : 1;
    if (a->seq != b->seq) return a->seq < b->seq ? -1 : 1;
    return 0;
}

static int ref_render(const RTri *tris, const RWireSpec *sp, int n, int w, int h,
                      unsigned char *rgb, int *blended_out) {
    int blended = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            RefLayer list[64];
            int k = 0;
            for (int i = 0; i < n; ++i)
                if (ref_covering(&tris[i], &sp[i], i, x + 0.5, y + 0.5, &list[k])) ++k;
            if (k == 0) continue;
            qsort(list, (size_t)k, sizeof(RefLayer), ref_cmp);
            double a = 0.0, pm[3] = {0, 0, 0};
            for (int j = 0; j < k; ++j) {
                double s = (double)list[j].opacity, keep = 1.0 - s;
                for (int c = 0; c < 3; ++c)
                    pm[c] = (double)list[j].rgb[c] * s + pm[c] * keep;
                a = s + a * keep;
            }
            for (int c = 0; c < 3; ++c) {
                /* dst starts at the same black frame in both paths */
                double v = pm[c] + 0.0 * (1.0 - a);
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                rgb[((size_t)y * w + x) * 3 + c] = (unsigned char)(v + 0.5);
            }
            ++blended;
        }
    *blended_out = blended;
    return 0;
}

static void test_against_bruteforce(void) {
    enum { W = 64, H = 64, N = 6 };
    RTri tris[N] = {TRI32, TRI64, TRI32, TRI64, TRI32, TRI64};
    RWireSpec sp[N] = {
        spec(2, 0.50f, 0.5, 200, 0, 0, 1),
        spec(0, 1.00f, 1.0, 0, 200, 0, 1),
        spec(1, 0.25f, 1.5, 0, 0, 200, 1),
        spec(2, 0.75f, 0.5, 200, 200, 0, 1),
        spec(1, 0.60f, 2.0, 0, 200, 200, 1),
        spec(0, 0.20f, 1.0, 200, 0, 200, 1),
    };
    RTarget t;
    CK(rt_init(&t, W, H) == 0);
    RWireResolveStats r; RWireErr e;
    CK(render_layers(&t, tris, sp, N, &r, &e) == RT_WIRE_OK);

    unsigned char *ref = (unsigned char*)calloc((size_t)W * H * 3, 1);
    CK(ref != NULL);
    int ref_blended = 0;
    CK(ref_render(tris, sp, N, W, H, ref, &ref_blended) == 0);
    CK(ref_blended == r.blend.blended);
    CK(ref_blended == r.merge.covered);
    int bad = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            for (int c = 0; c < 3; ++c) {
                int d = (int)t.rgb[i * 3 + c] - (int)ref[i * 3 + c];
                if (d < 0) d = -d;
                if (d > 1) { ++bad; break; }
            }
        }
    /* A single rounding difference is not expected (both round once from
     * the same doubles) but is tolerated at the last bit. */
    CK(bad == 0);
    /* Per-layer contribution counts add up: sum of layer_covered == total
     * number of (layer,pixel) pairs, and several pixels are covered twice
     * by the same layer's adjacent edges. */
    int sum = 0;
    for (int k = 0; k < r.merge.layers; ++k) sum += r.merge.order_covered[k];
    CK(sum == r.merge.contributions);
    CK(r.merge.contributions > r.merge.covered);
    free(ref);
    rt_free(&t);
}

int main(void) {
    test_edge_dist();
    test_stroke();
    test_blend_values();
    test_disabled_noop();
    test_errors();
    test_stage_separation();
    test_reproducible();
    test_against_bruteforce();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
