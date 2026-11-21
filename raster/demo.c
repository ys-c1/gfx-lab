#include "raster.h"
#include <stdio.h>
int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);
    /* 两段式抗锯齿画线：斜线不再断断续续 */
    RLine l1 = { 4.5, 4.5, 123.5, 123.5, 1.0, 256, 1, {255, 255, 255}, {0} };
    RLine l2 = { 4.5, 120.5, 123.5, 120.5, 2.0, 256, 1, {255, 220, 40}, {0} };
    RLine l3 = { 4.5, 60.5, 123.5, 90.5, 1.0, 256, 0, {40, 255, 120}, {0} }; /* 基线对照 */
    int c1 = rt_line(&t, &l1), c2 = rt_line(&t, &l2), c3 = rt_line(&t, &l3);
    printf("lines %d %d %d\n", c1, c2, c3);
    int rc = rt_write_ppm(&t, "demo.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
