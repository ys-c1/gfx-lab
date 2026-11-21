#include "raster.h"
#include <stdio.h>
int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);
    /* point sprites with perspective scaling: same size, growing depth */
    RSprite sp[4];
    for (int i = 0; i < 4; ++i) {
        sp[i].x = 24.0 + i * 28.0; sp[i].y = 64.0;
        sp[i].z = 1.0 + i; sp[i].size = 8.0;
        sp[i].rgb[0] = 250; sp[i].rgb[1] = 220; sp[i].rgb[2] = 60;
    }
    int ns = rt_sprites(&t, sp, 4, 24.0, NULL);
    printf("sprites %d\n", ns);
    int rc = rt_write_ppm(&t, "demo.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
