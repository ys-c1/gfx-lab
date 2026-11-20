#include "raster.h"
#include <stdio.h>
int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}, 1.0f};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}, 1.0f};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);

    /* second pass: two translucent triangles over the opaque scene */
    if (rt_set_blend(&t, 1) != RT_OK) return 1;
    RTri c = {{ {30,30,0.3}, {110,50,0.3}, {60,120,0.3} }, {250, 240, 60}, 0.5f};
    RTri d = {{ {0,80,0.1}, {90,60,0.1}, {40,127,0.1} }, {0, 220, 120}, 0.25f};
    RtStats sc, sd;
    if (rt_triangle_at(&t, &c, 2, &sc) < 0) return 1;
    if (rt_triangle_at(&t, &d, 3, &sd) < 0) return 1;
    printf("blend tri2 written=%d blended=%d\n", sc.written, sc.blended);
    printf("blend tri3 written=%d blended=%d\n", sd.written, sd.blended);

    int rc = rt_write_ppm(&t, "demo.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
