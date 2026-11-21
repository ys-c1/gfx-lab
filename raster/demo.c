#include "raster.h"
#include <stdio.h>
#include <string.h>

static RWireSpec spec(int layer, float opacity, double hw,
                      unsigned char r, unsigned char g, unsigned char b, int on) {
    RWireSpec s;
    s.layer = layer; s.opacity = opacity;
    s.rgb[0] = r; s.rgb[1] = g; s.rgb[2] = b;
    s.halfwidth = hw; s.enabled = on;
    return s;
}

int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("fill pixels %d %d\n", n1, n2);

    /* Overlay disabled: stroke/merge/blend must not change a byte, so
     * demo.ppm stays identical to the fill-only baseline. */
    RWireSpec off = spec(0, 1.0f, 1.5, 255, 255, 255, 0);
    RWireStrokeStats os; RWireErr e;
    if (rt_wire_stroke(&t, &a, &off, &os, &e) != RT_WIRE_OK) return 1;
    RWireResolveStats ors;
    if (rt_wire_resolve(&t, &ors, &e) != RT_WIRE_OK) return 1;
    printf("overlay off: layers=%d blended=%d\n", ors.merge.layers, ors.blend.blended);
    if (rt_write_ppm(&t, "demo.ppm") != 0) return 1;

    /* Overlay on: three layers over the same frame.  Stage 1 strokes
     * each in its own mask; stage 2 merges by layer number; stage 3
     * blends at its opacity.  Nothing is drawn into rgb until stage 3. */
    RWireSpec la = spec(1, 0.85f, 1.5, 255, 255, 255, 1); /* triangle a */
    RWireSpec lb = spec(2, 0.60f, 1.5, 255, 220, 0, 1);   /* triangle b */
    RWireSpec lc = spec(0, 1.00f, 0.0, 0, 0, 0, 1);       /* layer 0 first */
    RWireStrokeStats sa, sb, sc;
    int ca = rt_wire_stroke(&t, &a, &la, &sa, &e);
    int cb = rt_wire_stroke(&t, &b, &lb, &sb, &e);
    int cc = rt_wire_stroke(&t, &a, &lc, &sc, &e);
    if (ca < 0 || cb < 0 || cc < 0) {
        char msg[160];
        rt_wire_errfmt(&e, msg, sizeof msg);
        printf("wire error: %s\n", msg);
        return 1;
    }
    printf("stroke a: layer %d opacity %.2f covered %d edges [%d %d %d]\n",
           sa.layer, (double)sa.opacity, sa.covered,
           sa.per_edge[0], sa.per_edge[1], sa.per_edge[2]);
    printf("stroke b: layer %d opacity %.2f covered %d edges [%d %d %d]\n",
           sb.layer, (double)sb.opacity, sb.covered,
           sb.per_edge[0], sb.per_edge[1], sb.per_edge[2]);
    printf("stroke c: layer %d opacity %.2f covered %d edges [%d %d %d]\n",
           sc.layer, (double)sc.opacity, sc.covered,
           sc.per_edge[0], sc.per_edge[1], sc.per_edge[2]);

    RWireResolveStats r;
    if (rt_wire_resolve(&t, &r, &e) != RT_WIRE_OK) return 1;
    printf("merge: layers=%d covered=%d contributions=%d opaque=%d order",
           r.merge.layers, r.merge.covered, r.merge.contributions, r.merge.opaque);
    for (int k = 0; k < r.merge.layers; ++k)
        printf(" [L%d a=%.2f n=%d]", r.merge.order_layer[k],
               (double)r.merge.order_opacity[k], r.merge.order_covered[k]);
    printf("\nblend: blended=%d replaced=%d\n", r.blend.blended, r.blend.replaced);
    if (rt_write_ppm(&t, "demo_wire.ppm") != 0) return 1;
    rt_free(&t);
    return 0;
}
