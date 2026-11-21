#include "raster.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);
    int rc = rt_write_ppm(&t, "demo.ppm");
    if (rc != 0) { rt_free(&t); return 1; }

    /* Three-phase stencil demo: the circle's top-left quadrant, selected by a
     * bit mask, is shaded over the scene.  The circular occlusion therefore
     * comes from the stencil rather than from a depth trick. */
    RTStencilErrorInfo err;
    RTStencilStats st;
    if (rt_stencil_enable(&t, 1) != 0) { rt_free(&t); return 1; }
    rt_stencil_clear(&t, 0, &err);

    RTStencilShape disc = {64.0, 64.0, 40.0, 0x01, 0x01};
    if (rt_stencil_write(&t, &disc, &st, &err) != 0) {
        printf("write: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return 1;
    }
    int covered = st.covered, written = st.written;

    /* Stamp a second shape with bit 1 and test only bit 1, so the shaded set
     * is that shape intersected with the circle mask: a crescent. */
    RTStencilShape bar = {40.0, 40.0, 34.0, 0x02, 0x02};
    if (rt_stencil_write(&t, &bar, &st, &err) != 0) {
        printf("write: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return 1;
    }
    int covered2 = st.covered, written2 = st.written;

    RTStencilTest hit = {RT_STENCIL_FUNC_EQUAL, 0x02, 0x02};
    if (rt_stencil_test(&t, &hit, &st, &err) != 0) {
        printf("test: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return 1;
    }
    int passes = st.passes;

    RTStencilShade disc_col = {0.1, {250, 220, 90}};
    if (rt_stencil_shade(&t, &disc_col, &st, &err) != 0) {
        printf("shade: %s (%s)\n", rt_stencil_error_name(err.code),
               err.param ? err.param : "-");
        rt_free(&t);
        return 1;
    }
    printf("stencil: write1 covered %d written %d | write2 covered %d written %d | "
           "passes %d shaded %d\n",
           covered, written, covered2, written2, passes, st.shaded);
    int need = (128 * 128 + 7) / 8;
    unsigned char *all = (unsigned char*)malloc((size_t)need);
    if (all && rt_stencil_export_value_bits(&t, 1, all, need, &err) == 0) {
        int ones = 0;
        for (int i = 0; i < 128 * 128; ++i) ones += (all[i >> 3] >> (i & 7)) & 1;
        printf("bitmask bit 1: set %d of %d (first byte %02X)\n",
               ones, 128 * 128, all[0]);
    }
    free(all);

    rc = rt_write_ppm(&t, "demo_stencil.ppm");
    rt_free(&t);
    return rc == 0 ? 0 : 1;
}
