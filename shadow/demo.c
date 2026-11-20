#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

/* 基准场景（k=1 为 128，k=4 为 512）：
   光 (64.5k, 10.5k) r=40k；A: (20k,60k)-(108k,60k) tau=0.5；
   B: (44k,70k)-(84k,70k) tau=0.3。
   手算透光率：重叠区 T = 0.5*0.3 = 0.15，单层区 T = 0.5。 */

static double run_case(int k, ShStats *st, double *ov_err, double *sg_err,
                       double *ov_hand, double *sg_hand, int iters) {
    int n = 128 * k;
    ShadowMap m;
    if (sh_init(&m, n, n) != 0) return -1.0;
    ShLight l = {64.5f * k, 10.5f * k, 40.0f * k, 1.0f};
    ShSegT segs[2] = {
        {20.0f * k, 60.0f * k, 108.0f * k, 60.0f * k, 0.5f},
        {44.0f * k, 70.0f * k, 84.0f * k, 70.0f * k, 0.3f},
    };
    clock_t t0 = clock();
    for (int it = 0; it < iters; ++it)
        if (sh_render_t(&m, &l, segs, 2, st) != 0) { sh_free(&m); return -1.0; }
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / iters;

    /* 手算比对（双精度手算 vs 渲染结果）：重叠像素 (64k,100k)，单层像素 (100k,100k) */
    int ox = 64 * k, oy = 100 * k, sx = 100 * k, sy = 100 * k;
    double dx = ox + 0.5 - l.x, dy = oy + 0.5 - l.y;
    double fall = 1.0 / (1.0 + sqrt(dx * dx + dy * dy) / l.radius);
    *ov_hand = fall * 0.15;
    *ov_err = fabs((double)m.buf[(size_t)oy * n + ox] - *ov_hand);
    dx = sx + 0.5 - l.x; dy = sy + 0.5 - l.y;
    fall = 1.0 / (1.0 + sqrt(dx * dx + dy * dy) / l.radius);
    *sg_hand = fall * 0.5;
    *sg_err = fabs((double)m.buf[(size_t)sy * n + sx] - *sg_hand);

    if (k == 1) sh_write_pgm(&m, "demo.pgm");
    sh_free(&m);
    return ms;
}

int main(void) {
    printf("scene: A.tau=0.5 B.tau=0.3  hand: T_overlap=0.5*0.3=0.15  T_single=0.5\n");
    printf("%-4s %10s %10s %10s %12s %9s %12s %9s %9s\n",
           "res", "acc_err", "ov_err", "sg_err", "ov_mean", "ov_n",
           "sg_mean", "sg_n", "ms/render");
    for (int k = 1; k <= 4; k += 3) {
        ShStats st;
        double ov_err, sg_err, ov_hand, sg_hand;
        int iters = k == 1 ? 200 : 20;
        double ms = run_case(k, &st, &ov_err, &sg_err, &ov_hand, &sg_hand, iters);
        if (ms < 0.0) return 1;
        printf("%-4d %10.3g %10.3g %10.3g %12.6f %9d %12.6f %9d %9.3f\n",
               128 * k, (double)st.max_trans_err, ov_err, sg_err,
               st.mean_overlap, st.n_overlap, st.mean_single, st.n_single, ms);
        printf("     hand: overlap=%.7f single=%.7f\n", ov_hand, sg_hand);
    }
    return 0;
}
