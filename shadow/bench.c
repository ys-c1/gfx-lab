#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 确定性场景：nseg 条横竖交替的遮挡段 */
static void make_scene(ShSeg *segs, int nseg, int n) {
    for (int i = 0; i < nseg; ++i) {
        float fx = (float)((i * 37 + 11) % 100) / 100.0f;
        float fy = (float)((i * 53 + 29) % 100) / 100.0f;
        float len = (float)n * (0.1f + 0.2f * ((i * 13 + 7) % 10) / 10.0f);
        segs[i].x0 = fx * n;
        segs[i].y0 = fy * n;
        if (i % 2) { segs[i].x1 = segs[i].x0 + len; segs[i].y1 = segs[i].y0; }
        else       { segs[i].x1 = segs[i].x0;        segs[i].y1 = segs[i].y0 + len; }
    }
}

int main(void) {
    const int sizes[] = {256, 1024};
    const int blocks[] = {4, 16};
    const int nseg = 24;
    ShSeg *segs = (ShSeg *)malloc(sizeof(ShSeg) * (size_t)nseg);
    if (!segs) return 1;

    printf("%-6s %-6s %6s %10s %12s %12s | %14s %12s | %12s %12s %10s\n",
           "size", "block", "nseg", "reductions", "local_bytes", "reduced_bytes",
           "pre_post_diff", "pre_post_px", "err_max", "err_mean", "err_px");

    for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
        int n = sizes[si];
        make_scene(segs, nseg, n);
        ShLight l = {0.5f * n, 0.2f * n, 0.6f * n, 1.0f};

        ShadowMap ref, out;
        if (sh_init(&ref, n, n) != 0 || sh_init(&out, n, n) != 0) return 1;
        /* 对照：逐像素计算的结果 */
        if (sh_render(&ref, &l, segs, nseg) != 0) return 1;

        for (unsigned bi = 0; bi < sizeof(blocks) / sizeof(blocks[0]); ++bi) {
            ShConfig c;
            sh_config_default(&c);
            ShValue on = {SH_T_INT, 1, 0.0f};
            ShValue b = {SH_T_INT, blocks[bi], 0.0f};
            sh_config_set(&c, "segmented", on, NULL);
            sh_config_set(&c, "block", b, NULL);

            ShStats st;
            if (sh_render_ex(&out, &l, segs, nseg, &c, &st) != 0) return 1;

            double err_max = 0.0, err_sum = 0.0;
            long err_px = 0;
            for (long i = 0; i < (long)n * n; ++i) {
                double d = fabs((double)out.buf[i] - (double)ref.buf[i]);
                if (d > err_max) err_max = d;
                err_sum += d;
                if (d != 0.0) ++err_px;
            }
            printf("%-6d %-6d %6d %10ld %12ld %12ld | %14.1f %12ld | %12.6f %12.8f %10ld\n",
                   n, blocks[bi], st.segments, st.reductions,
                   st.local_bytes, st.reduced_bytes,
                   st.pre_post_diff, st.pre_post_cells,
                   err_max, err_sum / ((double)n * n), err_px);
        }
        sh_free(&ref);
        sh_free(&out);
    }
    free(segs);
    return 0;
}
