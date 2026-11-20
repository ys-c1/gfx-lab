#include "raster.h"
#include <stdio.h>
#include <string.h>

static int render_scene(int w, int h, RTCullMode mode, RTBatchStats *stats,
                        const char *path) {
    RTarget t;
    if (rt_init(&t, w, h) != 0) return -1;
    rt_set_cull(&t, mode);
    double s = (double)w / 128.0;
    RTri a = {{ {10*s,10*s,0.8}, {118*s,20*s,0.6}, {64*s,118*s,0.4} }, {200, 40, 40}};
    RTri b = {{ {20*s,100*s,0.7}, {110*s,90*s,0.9}, {70*s,20*s,0.2} }, {40, 180, 220}};
    RTri tris[2] = {a, b};
    int rc = rt_batch(&t, tris, 2, stats);
    if (rc == 0 && path) rc = rt_write_ppm(&t, path);
    rt_free(&t);
    return rc;
}

static void print_stats(const char *label, const RTBatchStats *st) {
    printf("%s: drawn %d culled %d rejected %d covered %lld filled %lld\n",
           label, st->drawn, st->culled, st->rejected, st->covered, st->filled);
    printf("%s: culled idx", label);
    for (int i = 0; i < st->culled_n; ++i) printf(" %d", st->culled_idx[i]);
    printf(" | rejected idx");
    for (int i = 0; i < st->rejected_n; ++i) printf(" %d", st->rejected_idx[i]);
    printf("\n");
    printf("%s: reasons", label);
    for (int r = 0; r < RT_CULL_REASON_COUNT; ++r)
        printf(" %s=%d", rt_cull_reason_name((RTCullReason)r),
               st->reason_counts[r]);
    printf("\n");
}

int main(void) {
    /* The default scene is rendered first with culling disabled so that
     * demo.ppm stays byte-for-byte identical to the pre-culling rasterizer. */
    RTarget t;
    if (rt_init(&t, 128, 128) != 0) return 1;
    RTri a = {{ {10,10,0.8}, {118,20,0.6}, {64,118,0.4} }, {200, 40, 40}};
    RTri b = {{ {20,100,0.7}, {110,90,0.9}, {70,20,0.2} }, {40, 180, 220}};
    int n1 = rt_triangle(&t, &a), n2 = rt_triangle(&t, &b);
    printf("pixels %d %d\n", n1, n2);
    int rc = rt_write_ppm(&t, "demo.ppm");
    rt_free(&t);
    if (rc != 0) return 1;

    RTWinding wa, wb;
    rt_classify(&a, NULL, NULL, &wa);
    rt_classify(&b, NULL, NULL, &wb);
    printf("tri 0 winding %s tri 1 winding %s\n",
           rt_winding_name(wa), rt_winding_name(wb));

    /* The same scene under each culling switch. */
    static const struct { RTCullMode mode; const char *name; } modes[] = {
        { RT_CULL_NONE, "none" },
        { RT_CULL_KEEP_CCW, "ccw" },
        { RT_CULL_KEEP_CW, "cw" },
        { RT_CULL_BOTH, "both" },
    };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        RTBatchStats st;
        char path[64];
        snprintf(path, sizeof path, "demo_%s.ppm", modes[i].name);
        if (render_scene(128, 128, modes[i].mode, &st, path) != 0) return 1;
        print_stats(modes[i].name, &st);
        rt_stats_free(&st);
    }
    return 0;
}
