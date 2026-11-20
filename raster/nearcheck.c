#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <stdio.h>

/* Triangle whose tip crosses the near plane: v2 sits behind near_z. */
int main(void) {
    RTri tri = {{ {4,4,0.6}, {28,4,0.6}, {16,16,-0.4} }, {255, 255, 255}};
    RTarget off, on;
    rt_init(&off, 32, 32);
    rt_init(&on, 32, 32);
    off.near_z = on.near_z = 0.25;

    RStats so;
    int rc_off = rt_draw(&off, &tri, 1, &so);
    on.clip = 1;
    RStats sn;
    int rc_on = rt_draw(&on, &tri, 1, &sn);

    printf("clip off: rc=%d (%s) err_tri=%d pixels=%ld tris_out=%ld\n",
           rc_off, rt_strerror(rc_off), so.err_tri, so.pixels, so.tris_out);
    printf("clip on : rc=%d (%s) err_tri=%d pixels=%ld tris_out=%ld new_verts=%ld\n",
           rc_on, rt_strerror(rc_on), sn.err_tri, sn.pixels, sn.tris_out, sn.new_verts);

    /* clipped quad spans y=4..8.2 (hits A=(8.2,8.2), B=(23.8,8.2));
       row y=5 (center 5.5) is inside it, lit only when clipping is on */
    int lit_off = 0, lit_on = 0;
    for (int x = 0; x < 32; ++x) {
        size_t i = (size_t)5 * 32 + (size_t)x;
        if (off.rgb[i * 3]) ++lit_off;
        if (on.rgb[i * 3]) ++lit_on;
    }
    printf("row y=5 lit pixels: clip-off=%d clip-on=%d\n", lit_off, lit_on);
    int ok = rc_off == RE_NEAR && so.err_tri == 0 && so.pixels == 0 &&
             rc_on == RE_OK && sn.new_verts == 2 && sn.tris_out == 2 &&
             lit_off == 0 && lit_on > 0;
    printf(ok ? "NEAR-PLANE SCENARIO OK\n" : "NEAR-PLANE SCENARIO FAIL\n");

    rt_write_ppm(&off, "off.ppm");
    rt_write_ppm(&on, "on.ppm");
    rt_free(&off);
    rt_free(&on);
    return ok ? 0 : 1;
}
