#include "shadow.h"
#include <stdio.h>
#include <math.h>
#include <time.h>

/* Stripe report for one slope scene at one resolution. */
static void report(int W, float bias, ShAcne *out, double *ms) {
    float scale = 128.0f / (float)W;
    ShadowMap m;
    ShError err;
    ShLight light = {65.0f * scale, 2.0f * scale, 60.0f * scale, 1.0f};
    ShSeg seg = {20.0f * scale, 30.0f * scale, 110.0f * scale, 50.0f * scale};
    clock_t t0, t1;
    sh_init_scaled(&m, W, W, scale);
    sh_set_bias(&m, bias, SH_DEFAULT_SLOPE_CLAMP, &err);
    t0 = clock();
    sh_render_ex(&m, &light, &seg, 1, &err);
    t1 = clock();
    sh_acne(&m, &light, &seg, 1, out, &err);
    *ms = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    sh_free(&m);
}

int main(void) {
    /* The original scene: two horizontal segments.  Horizontal surfaces take
     * zero offset, so with the default bias the picture is unchanged. */
    ShadowMap m;
    ShLight light = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    float sum = 0.0f;
    if (sh_init(&m, 128, 128) != 0) return 1;
    if (sh_render(&m, &light, segs, 2) != 0) return 1;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("original scene mean %.4f (was 0.2829)\n", sum / (128.0f * 128.0f));
    if (sh_write_pgm(&m, "demo.pgm") != 0) { sh_free(&m); return 1; }
    sh_free(&m);

    /* The slope that showed stripes: y = 0.222x, lit from directly above. */
    printf("\n  %-4s %-5s %8s %9s %9s %9s %8s %8s\n", "W", "bias",
           "stripes", "stripe_px", "offset_px", "needed_px", "error_px", "ms");
    for (int W = 128; W <= 512; W *= 4) {
        for (int on = 0; on <= 1; ++on) {
            float bias = on ? SH_DEFAULT_BIAS : 0.0f;
            float scale = 128.0f / (float)W;
            ShAcne a;
            double ms;
            ShError err;
            ShLight l = {65.0f * scale, 2.0f * scale, 60.0f * scale, 1.0f};
            ShSeg s = {20.0f * scale, 30.0f * scale, 110.0f * scale,
                       50.0f * scale};
            float needed = 0.0f;
            float offset;
            report(W, bias, &a, &ms);
            if (sh_init_scaled(&m, W, W, scale) != 0) return 1;
            sh_set_bias(&m, bias, SH_DEFAULT_SLOPE_CLAMP, &err);
            offset = sh_seg_offset(&m, &s, 0);
            sh_seg_needed_offset(&m, &l, &s, 0, &needed, &err);
            sh_free(&m);
            printf("  %-4d %-5g %8d %9d %9.3f %9.3f %+8.3f %8.3f\n", W,
                   (double)bias, a.stripes, a.shadowed, offset / scale,
                   needed / scale, offset / scale - needed / scale, ms);
        }
    }
    printf("(error = configured offset - offset the surface needs)\n");

    /* A vertical segment has no finite slope. */
    segs[0] = (ShSeg){60.0f, 20.0f, 60.0f, 110.0f};
    if (sh_init(&m, 128, 128) != 0) return 1;
    {
        ShError err;
        int rc = sh_render_ex(&m, &light, segs, 1, &err);
        printf("\nvertical segment: rc %d, bad field %s\n", rc, err.field);
    }
    sh_free(&m);
    return 0;
}
