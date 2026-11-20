#include "raster.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);

    /* Wireframe off: demo.ppm must be byte-identical to the fill-only output. */
    RWire off = {0.0, {255, 255, 255}, 0};
    RWireStats off_stats;
    RWireErr off_err;
    if (rt_wire(&t, &a, &off, &off_stats, &off_err) != RT_WIRE_OK) return 1;
    if (rt_write_ppm(&t, "demo.ppm") != 0) return 1;

    /* Wireframe on: overlay both triangles' edges at 1.5 px half width. */
    RWire w = {1.5, {255, 255, 255}, 1};
    RWireStats sa, sb;
    RWireErr e;
    int ca = rt_wire(&t, &a, &w, &sa, &e);
    int cb = rt_wire(&t, &b, &w, &sb, &e);
    if (ca < 0) { printf("wire a: %s\n", rt_wire_strerror(ca)); return 1; }
    if (cb < 0) { printf("wire b: %s\n", rt_wire_strerror(cb)); return 1; }
    printf("wire width %.1f px: a covered %d edges [%d %d %d]\n",
           rt_wire_nominal_width(&w), sa.covered, sa.edge[0], sa.edge[1], sa.edge[2]);
    printf("wire width %.1f px: b covered %d edges [%d %d %d]\n",
           rt_wire_nominal_width(&w), sb.covered, sb.edge[0], sb.edge[1], sb.edge[2]);
    int rc = rt_write_ppm(&t, "demo_wire.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
