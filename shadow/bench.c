#include "shadow.h"
#include <stdio.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void) {
    const int res[] = {128, 512};
    const int spp[] = {4, 16, 64};
    printf("%-5s %14s %14s %10s %12s %12s %8s\n",
           "res", "samples/pixel", "total_samples", "time_ms",
           "penumbra_px", "analytic_px", "err_px");
    for (size_t r = 0; r < sizeof(res) / sizeof(res[0]); ++r) {
        int W = res[r], H = res[r];
        ShadowMap m;
        if (sh_init(&m, W, H) != 0) return 1;
        float ly = 0.05f * H, yo = 0.40f * H;
        ShSeg occ = {0.45f * W, yo, 0.55f * W, yo};
        for (size_t s = 0; s < sizeof(spp) / sizeof(spp[0]); ++s) {
            ShAreaLight l = {0.5f * W, ly, 0.9f * W, 1.0f, 0.02f * W, spp[s]};
            double t0 = now_ms();
            if (sh_render_area(&m, &l, &occ, 1) != 0) return 1;
            double t1 = now_ms();
            float pw = 0.0f;
            if (sh_penumbra_width(&m, &l, &pw) != 0) return 1;
            float yg = (float)H - 0.5f;
            float analytic = 2.0f * l.size * (yg - yo) / (yo - ly);
            long long total = (long long)W * H * spp[s];
            printf("%-5d %14d %14lld %10.2f %12.1f %12.2f %8.2f\n",
                   W, spp[s], total, t1 - t0, pw, analytic, pw - analytic);
        }
        sh_free(&m);
    }

    /* 采样点分布可取到：打印 16 点分布的包围盒 */
    ShAreaLight l = {64.0f, 6.4f, 115.2f, 1.0f, 2.56f, 16};
    float pts[2 * 16];
    int n = 0;
    if (sh_area_points(&l, pts, 16, &n) != 0) return 1;
    float x0 = pts[0], x1 = pts[0], y0 = pts[1], y1 = pts[1];
    for (int k = 0; k < n; ++k) {
        if (pts[2 * k] < x0) x0 = pts[2 * k];
        if (pts[2 * k] > x1) x1 = pts[2 * k];
        if (pts[2 * k + 1] < y0) y0 = pts[2 * k + 1];
        if (pts[2 * k + 1] > y1) y1 = pts[2 * k + 1];
    }
    printf("sample distribution: n=%d x=[%.2f,%.2f] y=[%.2f,%.2f] (disc r=%.2f)\n",
           n, x0, x1, y0, y1, l.size);
    return 0;
}
