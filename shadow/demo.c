#include "shadow.h"
#include <stdio.h>

int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;

    /* Legacy path: unchanged, still produces demo.pgm. */
    ShLight one = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    if (sh_render(&m, &one, segs, 2) != 0) return 1;
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("mean %.4f\n", sum / (128.0f * 128.0f));
    int rc = sh_write_pgm(&m, "demo.pgm");

    /* Multi-light path: unsorted input, two lights, same scene. */
    ShLight lights[2] = {
        {100.0f, 8.0f, 26.0f, 0.4f},   /* dim */
        {40.0f, 12.0f, 40.0f, 1.0f},   /* bright */
    };
    ShMultiStats st;
    ShError err;
    if (sh_render_lights(&m, lights, 2, segs, 2, 1, &st, &err) != SH_OK) {
        printf("multi failed: %s: %s\n", err.field, err.msg);
        sh_free(&m);
        return 1;
    }
    sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("multi mean %.4f (order %d,%d; %d passes over %llu pixels)\n",
           sum / (128.0f * 128.0f), st.order[0].index, st.order[1].index,
           st.passes, st.pixels);
    if (rc == 0) rc = sh_write_pgm(&m, "demo_multi.pgm");
    if (rc == 0) rc = sh_write_order_table("demo_order.csv", st.order, st.lights,
                                           "demo", 128, 128);

    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
