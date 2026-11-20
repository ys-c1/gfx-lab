#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Hand-worked fixture (see final report for the arithmetic):
 *   v0=(4.5,4.5) v1=(28.5,4.5) v2=(16.5,28.5), target 32x32, T=0.5
 * scaled 4x with T=2 into a 128x128 target:
 *   v0=(16.5,16.5) v1=(112.5,16.5) v2=(64.5,112.5)
 * Per-edge covered-centre counts and distinct covered pixels are asserted
 * exactly, so any drift in distance or thresholding shows up. */
static const RTri TRI32 = {{ {4.5,4.5,0.5}, {28.5,4.5,0.5}, {16.5,28.5,0.5} }, {255, 0, 0}};
static const RTri TRI128 = {{ {16.5,16.5,0.5}, {112.5,16.5,0.5}, {64.5,112.5,0.5} }, {255, 0, 0}};

static int pixel_eq(const unsigned char *a, const unsigned char *b, int n) {
    return memcmp(a, b, (size_t)n * 3) == 0;
}

static void test_edge_dist_stage1(void) {
    RVert a = {4.5, 4.5, 0.0}, b = {28.5, 4.5, 0.0};
    double d = -1.0;
    /* Pixel centres are at integer+0.5. */
    CK(rt_edge_dist(&a, &b, 16, 4, &d) == RT_WIRE_OK && fabs(d - 0.0) < 1e-12);
    CK(rt_edge_dist(&a, &b, 16, 5, &d) == RT_WIRE_OK && fabs(d - 1.0) < 1e-12);
    CK(rt_edge_dist(&a, &b, 16, 2, &d) == RT_WIRE_OK && fabs(d - 2.0) < 1e-12);
    /* Past the endpoint the projection clamps: nearest point is v1=(28.5,4.5),
     * pixel (30,4) centre (30.5,4.5) -> 2.0, not distance to the infinite line. */
    CK(rt_edge_dist(&a, &b, 30, 4, &d) == RT_WIRE_OK && fabs(d - 2.0) < 1e-12);
    /* Vertex exactly on a centre. */
    CK(rt_edge_dist(&a, &b, 4, 4, &d) == RT_WIRE_OK && fabs(d - 0.0) < 1e-12);
    /* Diagonal edge v1-v2: |(28.5,4.5)-(16.5,28.5)| = sqrt(12^2+24^2). */
    double expect = sqrt(12.0 * 12.0 + 24.0 * 24.0) / sqrt(12.0 * 12.0 + 24.0 * 24.0);
    CK(rt_edge_dist(&TRI32.v[1], &TRI32.v[2], 22, 16, &d) == RT_WIRE_OK && d <= expect);
    /* Degenerate segment is rejected before dividing by zero. */
    RVert same = {7.0, 7.0, 0.0};
    CK(rt_edge_dist(&same, &same, 0, 0, &d) == RT_WIRE_ERR_DUP_VERTEX);
    CK(rt_edge_dist(NULL, &b, 0, 0, &d) == RT_WIRE_ERR_ARG);
}

static void test_threshold_stage2(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RWire w = {0.5, {0, 255, 0}, 1};
    RWireStats s;
    RWireErr e;
    CK(rt_wire(&t, &TRI32, &w, &s, &e) == 95);
    CK(s.edge[0] == 25 && s.edge[1] == 37 && s.edge[2] == 37);
    CK(s.covered == 95);
    CK(rt_wire_nominal_width(&w) == 1.0);
    /* Pixel centres exactly on an edge qualify; one pixel off do not. */
    CK(t.wire[4*32+16] == 1);
    CK(t.wire[3*32+16] == 0);
    CK(t.rgb[(4*32+16)*3+1] == 255 && t.rgb[(4*32+16)*3+0] == 0);
    /* Outside the bounding-box margin? nothing drawn on the far left. */
    CK(t.wire[4*32+0] == 0);

    /* Half-width 0 still draws centres exactly on the edge (d <= 0). */
    RTarget z;
    CK(rt_init(&z, 32, 32) == 0);
    RWire w0 = {0.0, {9, 9, 9}, 1};
    int n0 = rt_wire(&z, &TRI32, &w0, &s, &e);
    CK(n0 > 0 && s.edge[0] > 0);
    rt_free(&z);
    rt_free(&t);

    /* 128-scale fixture: 4x geometry, 2x threshold. */
    RTarget t2;
    CK(rt_init(&t2, 128, 128) == 0);
    RWire w2 = {2.0, {0, 255, 0}, 1};
    CK(rt_wire(&t2, &TRI128, &w2, &s, &e) == 1332);
    CK(s.edge[0] == 493 && s.edge[1] == 445 && s.edge[2] == 445);
    CK(s.covered == 1332);

    /* Layer/thickness probe on the horizontal edge at its midpoint column. */
    int thick = 0;
    for (int y = 0; y < 128; ++y) {
        double d;
        CK(rt_edge_dist(&TRI128.v[0], &TRI128.v[1], 60, y, &d) == RT_WIRE_OK);
        if (d <= w2.threshold) ++thick;
    }
    CK(thick == 5);  /* nominal 4 px; centres at y=14..18 inclusive */
    rt_free(&t2);
}

static void test_disabled_is_noop(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = TRI32;
    CK(rt_triangle(&t, &tri) > 0);
    size_t n = (size_t)t.w * (size_t)t.h;
    float *zcopy = (float*)malloc(sizeof(float) * n);
    unsigned char *rgbcopy = (unsigned char*)malloc(n * 3);
    unsigned char *wirecopy = (unsigned char*)malloc(n);
    CK(zcopy && rgbcopy && wirecopy);
    memcpy(zcopy, t.z, sizeof(float) * n);
    memcpy(rgbcopy, t.rgb, n * 3);
    memcpy(wirecopy, t.wire, n);

    /* Disabled must skip validation entirely: negative threshold and a
     * degenerate triangle are both ignored, and nothing is written. */
    RWire off = {-5.0, {255, 255, 255}, 0};
    RTri degenerate = {{ {1,1,0}, {1,1,0}, {9,9,0} }, {0, 0, 0}};
    RWireStats s;
    RWireErr e;
    CK(rt_wire(&t, &tri, &off, &s, &e) == RT_WIRE_OK);
    CK(rt_wire(&t, &degenerate, &off, &s, &e) == RT_WIRE_OK);
    CK(memcmp(zcopy, t.z, sizeof(float) * n) == 0);
    CK(pixel_eq(rgbcopy, t.rgb, (int)n));
    CK(memcmp(wirecopy, t.wire, n) == 0);

    /* Toggling off after drawing leaves the previous overlay in place. */
    RWire on = {0.5, {0, 255, 0}, 1};
    CK(rt_wire(&t, &tri, &on, &s, &e) == 95);
    memcpy(rgbcopy, t.rgb, n * 3);
    CK(rt_wire(&t, &tri, &off, &s, &e) == RT_WIRE_OK);
    CK(pixel_eq(rgbcopy, t.rgb, (int)n));

    /* The overlay never writes depth: z survives the wire pass byte for byte. */
    memcpy(zcopy, t.z, sizeof(float) * n);
    CK(rt_wire(&t, &tri, &on, &s, &e) == 95);
    CK(memcmp(zcopy, t.z, sizeof(float) * n) == 0);

    /* Huge threshold must clamp to the target, not overflow an int cast. */
    RWire huge = {1e9, {255, 255, 255}, 1};
    CK(rt_wire(&t, &tri, &huge, &s, &e) == (int)n);
    CK(s.covered == (int)n);

    /* Fully off-screen triangle: no pixels, but validation still runs. */
    RWire small = {0.5, {0, 255, 0}, 1};
    RTri away = {{ {-500,-500,0}, {-480,-500,0}, {-500,-480,0} }, {0, 0, 0}};
    CK(rt_wire(&t, &away, &small, &s, &e) == 0);
    CK(s.covered == 0 && s.edge[0] == 0);

    free(wirecopy); free(rgbcopy); free(zcopy);
    rt_free(&t);
}

static void test_errors(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RWire w = {0.5, {0, 255, 0}, 1};
    RWireStats s;
    RWireErr e;
    RTri tri = TRI32;

    /* Negative threshold. */
    RWire bad = {-0.25, {0, 255, 0}, 1};
    CK(rt_wire(&t, &tri, &bad, &s, &e) == RT_WIRE_ERR_THRESHOLD);
    CK(e.code == RT_WIRE_ERR_THRESHOLD);
    char msg[128];
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0);
    CK(strstr(msg, "threshold") != NULL);

    /* Coincident vertices, every pair.  Edge i is v[i] -> v[(i+1)%3]. */
    for (int i = 0; i < 3; ++i) {
        RTri d = TRI32;
        d.v[i] = d.v[(i + 1) % 3];
        CK(rt_wire(&t, &d, &w, &s, &e) == RT_WIRE_ERR_DUP_VERTEX);
        CK(e.code == RT_WIRE_ERR_DUP_VERTEX);
        CK(e.edge == i);
        CK(e.vertex == i && e.vertex2 == (i + 1) % 3);
        CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0);
        char want[64];
        snprintf(want, sizeof want, "edge %d (v%d-v%d)", i, i, (i + 1) % 3);
        CK(strstr(msg, want) != NULL);
    }

    /* Short edge: 1e-7 px, below RT_WIRE_MIN_EDGE.  Only edge 0 is short. */
    RTri sh = TRI32;
    sh.v[1].x = sh.v[0].x + 1e-7;
    sh.v[1].y = sh.v[0].y;
    CK(rt_wire(&t, &sh, &w, &s, &e) == RT_WIRE_ERR_SHORT_EDGE);
    CK(e.edge == 0 && e.vertex == 0 && e.vertex2 == 1);
    CK(rt_wire_errfmt(&e, msg, sizeof msg) == 0 && strstr(msg, "v0-v1") != NULL);
    /* Same shortness on edge 2 (v2->v0) is reported with its own numbers. */
    RTri sh2 = TRI32;
    sh2.v[2].x = sh2.v[0].x;
    sh2.v[2].y = sh2.v[0].y + 1e-8;
    CK(rt_wire(&t, &sh2, &w, &s, &e) == RT_WIRE_ERR_SHORT_EDGE);
    CK(e.edge == 2 && e.vertex == 2 && e.vertex2 == 0);

    /* Vertex order is never permuted: edge 0 must stay v0->v1 even when the
     * triangle is given clockwise (which the fill pass would still accept). */
    RTri cw = {{ {4.5,4.5,0.5}, {16.5,28.5,0.5}, {28.5,4.5,0.5} }, {255, 0, 0}};
    CK(rt_wire(&t, &cw, &w, &s, &e) == 95);
    CK(s.edge[0] == 37 && s.edge[1] == 37 && s.edge[2] == 25);

    /* The input triangle is left byte-identical. */
    RTri snapshot = TRI32;
    CK(rt_wire(&t, &tri, &w, &s, &e) == 95);
    CK(memcmp(&snapshot, &tri, sizeof tri) == 0);

    CK(rt_wire(NULL, &tri, &w, &s, &e) == RT_WIRE_ERR_ARG);
    CK(rt_wire(&t, NULL, &w, &s, &e) == RT_WIRE_ERR_ARG);
    rt_free(&t);
}

int main(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    test_edge_dist_stage1();
    test_threshold_stage2();
    test_disabled_is_noop();
    test_errors();

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
