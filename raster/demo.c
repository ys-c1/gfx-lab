#include "raster.h"
#include <stdio.h>
int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    double table[] = {1.0, 0.55, 0.85};
    RTint tint = { table, 3, {0, 1, 2} };
    int n1 = rt_triangle_tint(&t, &a, &tint); /* 顶点色调渐变 */
    int n2 = rt_triangle(&t, &b);             /* 不启用：输出保持原样 */
    printf("pixels %d %d\n", n1, n2);
    int rc = rt_write_ppm(&t, "demo.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
