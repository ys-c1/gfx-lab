#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int has_vertex(const RTri *tris, int n, double x, double y, double z) {
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            RVert v = tris[i].v[k];
            if (fabs(v.x - x) < 1e-9 && fabs(v.y - y) < 1e-9 && fabs(v.z - z) < 1e-9)
                return 1;
        }
    return 0;
}

int main(void) {
    /* ---- legacy behavior kept ---- */
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    CK(t.clip == 0 && t.near_z == 0.0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == RE_DEGEN);
    rt_free(&t);

    /* ---- clip hand check A: 1 vertex inside, near = 1 ----
       v0=(0,0,2) v1=(4,0,0) v2=(0,4,0).
       edge v0->v1, t=(1-2)/(0-2)=0.5 -> hit A=(2,0,1)
       edge v2->v0, t=(1-0)/(2-0)=0.5 -> hit B=(0,2,1)
       surviving fan: v0,A,B -> 1 triangle, 2 new vertices. */
    RTri a = {{ {0,0,2}, {4,0,0}, {0,4,0} }, {10, 20, 30}};
    RTri out[4]; int nv = -1;
    int rc = rt_clip_triangle(&a, 1.0, out, 4, &nv);
    CK(rc == 1);
    CK(nv == 2);
    CK(has_vertex(out, rc, 0, 0, 2));
    CK(has_vertex(out, rc, 2, 0, 1));
    CK(has_vertex(out, rc, 0, 2, 1));
    CK(out[0].rgb[0] == 10 && out[0].rgb[2] == 30);

    /* ---- clip hand check B: 2 vertices inside, near = 1 ----
       v0=(0,0,2) v1=(8,0,2) v2=(4,4,0).
       edge v1->v2, t=(1-2)/(0-2)=0.5 -> A=(6,2,1)
       edge v2->v0, t=(1-0)/(2-0)=0.5 -> B=(2,2,1)
       polygon v0,v1,A,B -> 2 fan triangles, 2 new vertices. */
    RTri b = {{ {0,0,2}, {8,0,2}, {4,4,0} }, {1, 2, 3}};
    rc = rt_clip_triangle(&b, 1.0, out, 4, &nv);
    CK(rc == 2);
    CK(nv == 2);
    CK(has_vertex(out, rc, 0, 0, 2));
    CK(has_vertex(out, rc, 8, 0, 2));
    CK(has_vertex(out, rc, 6, 2, 1));
    CK(has_vertex(out, rc, 2, 2, 1));

    /* ---- clip hand check C: vertex exactly on the plane ----
       v0=(0,0,1) on, v1=(4,0,1) on, v2=(0,4,3) inside: nothing is new. */
    RTri c = {{ {0,0,1}, {4,0,1}, {0,4,3} }, {0, 0, 0}};
    rc = rt_clip_triangle(&c, 1.0, out, 4, &nv);
    CK(rc == 1);
    CK(nv == 0);
    CK(has_vertex(out, rc, 0, 0, 1));
    CK(has_vertex(out, rc, 4, 0, 1));
    CK(has_vertex(out, rc, 0, 4, 3));

    /* ---- crossing where one edge hit lands on an input vertex ----
       v0=(0,0,1) on, v1=(4,0,0) out, v2=(0,4,0) out.
       only the single on-plane vertex survives contact -> RE_EMPTY, 0 new */
    RTri d = {{ {0,0,1}, {4,0,0}, {0,4,0} }, {0, 0, 0}};
    rc = rt_clip_triangle(&d, 1.0, out, 4, &nv);
    CK(rc == RE_EMPTY);
    CK(nv == 0);

    /* all vertices fully inside: unchanged, 0 new */
    rc = rt_clip_triangle(&a, 0.0, out, 4, &nv);
    CK(rc == 1);
    CK(nv == 0);
    CK(has_vertex(out, rc, 0, 0, 2) && has_vertex(out, rc, 4, 0, 0) && has_vertex(out, rc, 0, 4, 0));

    /* ---- error: clipping leaves nothing, triangle index reported ---- */
    RTri behind = {{ {0,0,0.1}, {4,0,0.2}, {0,4,0.3} }, {0, 0, 0}};
    CK(rt_clip_triangle(&behind, 1.0, out, 4, &nv) == RE_EMPTY);

    /* ---- error: coincident vertices, triangle index reported ---- */
    RTri degen = {{ {0,0,2}, {0,0,2}, {0,4,0} }, {0, 0, 0}};
    CK(rt_clip_triangle(&degen, 1.0, out, 4, &nv) == RE_DEGEN);
    CK(rt_clip_triangle(NULL, 1.0, out, 4, &nv) == RE_ARG);

    /* ---- rasterized fill count, hand count ----
       triangle (0,0),(8,0),(0,8) on a 16x16 target.
       pixel center (x+.5,y+.5) passes iff x+y+1 <= 8 -> x+y <= 7.
       rows y=0..7 give 8,7,...,1 pixels: sum = 36. */
    RTarget r;
    CK(rt_init(&r, 16, 16) == 0);
    RTri fill = {{ {0,0,0.5}, {8,0,0.5}, {0,8,0.5} }, {9, 9, 9}};
    int px = rt_triangle(&r, &fill);
    CK(px == 36);
    rt_free(&r);

    /* ---- clipping off vs on produce the same image when geometry survives ---- */
    RTarget t1, t2;
    CK(rt_init(&t1, 32, 32) == 0);
    CK(rt_init(&t2, 32, 32) == 0);
    RTri safe1 = {{ {2,2,0.5}, {30,2,0.5}, {16,30,0.5} }, {120, 60, 200}};
    RTri safe2 = {{ {6,26,0.3}, {26,24,0.8}, {18,6,0.4} }, {20, 200, 90}};
    RStats s1;
    CK(rt_draw(&t1, (RTri[]){safe1, safe2}, 2, &s1) == RE_OK);
    CK(s1.tris_in == 2 && s1.tris_out == 2 && s1.new_verts == 0 && s1.err_tri == -1);
    t2.clip = 1; t2.near_z = 0.0;
    RStats s2;
    CK(rt_draw(&t2, (RTri[]){safe1, safe2}, 2, &s2) == RE_OK);
    CK(memcmp(t1.rgb, t2.rgb, (size_t)32 * 32 * 3) == 0);
    CK(memcmp(t1.z, t2.z, (size_t)32 * 32 * sizeof(float)) == 0);
    CK(s1.pixels == s2.pixels);

    /* ---- error paths through rt_draw, index in err_tri ---- */
    RTri cross = {{ {2,2,0.5}, {30,2,0.5}, {16,30,-0.5} }, {1, 1, 1}};
    RStats se;
    /* clip off: depth below near plane -> RE_NEAR, triangle 0 */
    CK(rt_draw(&t1, (RTri[]){cross}, 1, &se) == RE_NEAR);
    CK(se.err_tri == 0 && se.tris_out == 0);
    /* triangle 1 crosses, triangle 0 is fine */
    CK(rt_draw(&t1, (RTri[]){safe1, cross}, 2, &se) == RE_NEAR);
    CK(se.err_tri == 1 && se.tris_out == 1);
    /* clip on: all-behind triangle -> RE_EMPTY */
    t1.clip = 1; t1.near_z = 1.0;
    CK(rt_draw(&t1, (RTri[]){behind}, 1, &se) == RE_EMPTY);
    CK(se.err_tri == 0);
    /* clip on: crossing triangle now succeeds and adds 2 new verts */
    RStats sc;
    t1.near_z = 0.0;
    RTri cross0 = {{ {2,2,0.5}, {30,2,0.5}, {16,30,-0.5} }, {5, 5, 5}};
    CK(rt_draw(&t1, (RTri[]){cross0}, 1, &sc) == RE_OK);
    /* quad survives: 2 intersection verts, fan-split into 2 triangles */
    CK(sc.new_verts == 2 && sc.tris_out == 2 && sc.pixels > 0);
    /* coincident vertex -> RE_DEGEN with triangle number */
    t1.clip = 0;
    CK(rt_draw(&t1, (RTri[]){safe1, degen}, 2, &se) == RE_DEGEN);
    CK(se.err_tri == 1);
    /* zero count is a valid no-op */
    CK(rt_draw(&t1, NULL, 0, &se) == RE_OK && se.tris_in == 0);
    rt_free(&t1);
    rt_free(&t2);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
