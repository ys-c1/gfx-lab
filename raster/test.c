#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int same_target(const RTarget *a, const RTarget *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    return memcmp(a->z, b->z, sizeof(float) * (size_t)a->w * (size_t)a->h) == 0 &&
           memcmp(a->rgb, b->rgb, (size_t)a->w * (size_t)a->h * 3) == 0;
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

    /* Disabled alignment must reproduce the legacy per-vertex result exactly. */
    RTri odd[3] = {
        {{ {4.37, 4.19, 0.5}, {27.61, 4.73, 0.4}, {15.52, 27.88, 0.6} }, {200, 10, 10}},
        {{ {0.13, 0.41, 0.9}, {31.77, 3.02, 0.2}, {2.51, 30.49, 0.7} }, {10, 200, 10}},
        {{ {8.49, 9.51, 0.3}, {9.51, 9.49, 0.3}, {8.5, 30.5, 0.3} }, {10, 10, 200}},
    };
    RTAlign off = {0, RT_ALIGN_HALF_PX};
    for (int i = 0; i < 3; ++i) {
        RTarget a, b;
        CK(rt_init(&a, 32, 32) == 0 && rt_init(&b, 32, 32) == 0);
        int legacy = rt_triangle(&a, &odd[i]);
        RTAlignReport rep;
        int aligned = rt_triangle_aligned(&b, &odd[i], &off, &rep);
        CK(aligned == legacy);
        CK(same_target(&a, &b));
        CK(rep.code == RT_OK && rep.vertex == -1);
        CK(rep.dx[0] == 0.0 && rep.dy[2] == 0.0 && rep.max_displacement == 0.0);
        rt_free(&a); rt_free(&b);
    }

    /* A grid of 0 (or negative/NaN) is rejected, and says so. */
    RTAlign zgrid = {1, 0.0};
    RTAlignReport rep;
    RTri probe = {{ {4,4,0}, {20,4,0}, {12,20,0} }, {1, 2, 3}};
    CK(rt_triangle_aligned(&t, &probe, &zgrid, &rep) == RT_E_GRID);
    CK(rep.code == RT_E_GRID && rep.vertex == -1 && rep.msg[0] != '\0');
    CK(rt_align_vertices(&probe, &zgrid, &tri, &rep) == RT_E_GRID);
    RTAlign ngrid = {1, -0.5};
    CK(rt_align_vertices(&probe, &ngrid, &tri, &rep) == RT_E_GRID);
    RTAlign nan_grid = {1, NAN};
    CK(rt_align_vertices(&probe, &nan_grid, &tri, &rep) == RT_E_GRID);

    /* A degenerate triangle is rejected before any snapping, naming no vertex. */
    RTAlign half = {1, RT_ALIGN_HALF_PX};
    RTri flat_al = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_align_vertices(&flat_al, &half, &tri, &rep) == RT_E_DEGEN);
    CK(rep.vertex == -1 && strstr(rep.msg, "zero area") != NULL);
    CK(rt_triangle_aligned(&t, &flat_al, &half, &rep) == RT_E_DEGEN);

    /* Overflow names the offending vertex. */
    RTri over = {{ {4,4,0}, {RT_MAX_COORD * 2.0, 4, 0}, {12,20,0} }, {1, 2, 3}};
    RTri out;
    CK(rt_align_vertices(&over, &half, &out, &rep) == RT_E_OVERFLOW);
    CK(rep.code == RT_E_OVERFLOW && rep.vertex == 1);
    CK(strstr(rep.msg, "vertex 1") != NULL);
    over.v[2].y = -INFINITY;
    over.v[1].x = 20;
    CK(rt_align_vertices(&over, &half, &out, &rep) == RT_E_OVERFLOW && rep.vertex == 2);

    /* A grid so tiny that the snap offset itself overflows is an overflow of
     * the vertex it affects, not a silent infinity in the scan. */
    RTAlign tiny_grid = {1, 1e-320};
    CK(rt_align_vertices(&probe, &tiny_grid, &out, &rep) == RT_E_OVERFLOW);
    CK(rep.vertex == 0 && strstr(rep.msg, "vertex 0") != NULL);

    /* Missing coordinates (NaN) name the offending vertex too. */
    RTri miss = {{ {4,4,0}, {20,4,0}, {NAN, 20, 0} }, {1, 2, 3}};
    CK(rt_align_vertices(&miss, &half, &out, &rep) == RT_E_MISSING);
    CK(rep.code == RT_E_MISSING && rep.vertex == 2);
    CK(strstr(rep.msg, "vertex 2") != NULL);
    miss.v[0].z = NAN;
    CK(rt_align_vertices(&miss, &half, &out, &rep) == RT_E_MISSING && rep.vertex == 0);

    /* Offsets are reported per vertex, snapped - input, and within half a grid. */
    RTri sub = {{ {4.3, 4.7, 0.5}, {27.6, 4.1, 0.5}, {15.8, 27.4, 0.5} }, {9, 9, 9}};
    CK(rt_align_vertices(&sub, &half, &out, &rep) == RT_OK);
    CK(rep.code == RT_OK && rep.vertex == -1);
    for (int i = 0; i < 3; ++i) {
        CK(fabs(rep.dx[i]) <= 0.25 + 1e-12 && fabs(rep.dy[i]) <= 0.25 + 1e-12);
        CK(fabs(out.v[i].x - (sub.v[i].x + rep.dx[i])) < 1e-12);
        CK(fabs(out.v[i].y - (sub.v[i].y + rep.dy[i])) < 1e-12);
        CK(fabs(out.v[i].x / 0.5 - round(out.v[i].x / 0.5)) < 1e-12);
        CK(fabs(out.v[i].y / 0.5 - round(out.v[i].y / 0.5)) < 1e-12);
        CK(out.v[i].z == sub.v[i].z);
    }
    CK(fabs(rep.dx[0] - 0.2) < 1e-12 && fabs(rep.dy[0] + 0.2) < 1e-12);
    CK(fabs(rep.max_displacement - 0.2) < 1e-12);

    /* Snapping can collapse a sliver that was valid at float positions; that
     * is reported, not treated as an error, and it renders as zero pixels. */
    RTri sliver = {{ {0.2, 0.2, 0}, {0.7, 0.2, 0}, {0.45, 0.24, 0} }, {1, 1, 1}};
    RTri sliver_out;
    CK(rt_align_vertices(&sliver, &half, &sliver_out, &rep) == RT_OK);
    CK(rep.collapsed == 1 && rep.vertex == -1);
    CK(strstr(rep.msg, "no area after snapping") != NULL);

    /* A rejected aligned call must not disturb the target; a collapsed one
     * writes nothing, too. */
    RTarget before, after;
    CK(rt_init(&before, 32, 32) == 0 && rt_init(&after, 32, 32) == 0);
    rt_triangle(&before, &tri);
    memcpy(after.z, before.z, sizeof(float) * 32 * 32);
    memcpy(after.rgb, before.rgb, 32 * 32 * 3);
    CK(rt_triangle_aligned(&before, &miss, &half, &rep) == RT_E_MISSING);
    CK(same_target(&before, &after));
    CK(rt_triangle_aligned(&before, &sliver, &half, &rep) == 0);
    CK(rep.collapsed == 1);
    CK(same_target(&before, &after));
    rt_free(&before); rt_free(&after);

    /* Aligned rendering stays close in coverage to the legacy result. */
    RTarget la, al;
    CK(rt_init(&la, 32, 32) == 0 && rt_init(&al, 32, 32) == 0);
    int nl = rt_triangle(&la, &odd[0]);
    int na = rt_triangle_aligned(&al, &odd[0], &half, &rep);
    CK(na > 0 && nl > 0);
    CK(abs(na - nl) < nl / 5);
    rt_free(&la); rt_free(&al);

    rt_free(&t);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
