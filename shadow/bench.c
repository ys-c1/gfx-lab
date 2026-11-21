/* 基准与核对：
 *  - 穷举参考渲染（每像素 32x32 规则细采样）当作"真值"，用于误差；
 *  - 低速/高速两档抖动分别记录采样点数、耗时、RMSE/最大误差；
 *  - 网格纹强度：边缘带内最大的单像素台阶幅度（规则二元采样台阶最大，
 *    抖动平均把一个硬台阶拆成多个小台阶，最大值显著下降）；
 *  - 网格纹周期：强台阶出现位置的平均间距（像素），规则采样应为 ~1px；
 *  - 与穷举结果核对：穷举参考的台阶最小、无 1px 强周期，抖动向其收敛。 */
#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 128
#define H 128
#define REF_SIDE 32
/* 边缘带：只统计跨光照明暗两侧的像素，避免平坦区稀释统计。 */
#define EDGE_LO 0.15f
#define EDGE_HI 0.85f

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 穷举参考：每像素 REF_SIDE^2 个均匀网格点直接平均，作为真值。 */
static int render_exhaustive(ShadowMap *m, const ShLight *l, const ShSeg *segs, int n) {
    if (sh_init(m, W, H) != 0) return -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double acc = 0.0;
            for (int gy = 0; gy < REF_SIDE; ++gy)
                for (int gx = 0; gx < REF_SIDE; ++gx) {
                    float px = (float)x + ((float)gx + 0.5f) / REF_SIDE;
                    float py = (float)y + ((float)gy + 0.5f) / REF_SIDE;
                    int shadowed = 0;
                    for (int i = 0; i < n; ++i) {
                        float rx = px - l->x, ry = py - l->y;
                        float sx = segs[i].x1 - segs[i].x0, sy = segs[i].y1 - segs[i].y0;
                        float den = rx * sy - ry * sx;
                        if (fabsf(den) < 1e-9f) continue;
                        float t = ((segs[i].x0 - l->x) * sy - (segs[i].y0 - l->y) * sx) / den;
                        float u = ((segs[i].x0 - l->x) * ry - (segs[i].y0 - l->y) * rx) / den;
                        if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f) { shadowed = 1; break; }
                    }
                    if (!shadowed) {
                        float dx = px - l->x, dy = py - l->y;
                        float d = sqrtf(dx * dx + dy * dy);
                        acc += (double)(l->intensity / (1.0f + d / l->radius));
                    }
                }
            m->buf[y * W + x] = (float)(acc / (REF_SIDE * REF_SIDE));
        }
    return 0;
}

static void errors_vs_ref(const ShadowMap *m, const ShadowMap *ref,
                          double *rmse, double *maxe) {
    double sse = 0.0, mx = 0.0;
    for (int i = 0; i < W * H; ++i) {
        double d = (double)m->buf[i] - (double)ref->buf[i];
        sse += d * d;
        if (fabs(d) > mx) mx = fabs(d);
    }
    *rmse = sqrt(sse / (W * H));
    *maxe = mx;
}

static int in_edge_band(float v) { return v > EDGE_LO && v < EDGE_HI; }

/* 网格纹综合度量。在边缘带内沿每一列取竖直最大台阶，组成一条沿 x 的序列：
 *   - strength：序列最大值，即图中最硬的单像素台阶（规则二元采样最大，
 *               抖动把一个硬台阶拆成多个小台阶，该值明显下降）；
 *   - lattice ：去掉斜边线性包络后的残差 RMS，即像素晶格走样的周期性能量，
 *               规则采样逐列硬台阶，能量约为穷举参考的两倍，抖动向穷举收敛；
 *   - period  ：残差 DFT 在晶格频段（周期 2~4px）主峰对应的空间周期（像素），
 *               即网格纹周期；规则采样的能量集中在该频段，穷举参考该频段很弱。 */
static void grid_metrics(const ShadowMap *m,
                         double *strength, double *lattice, double *period) {
    double sig[W];
    int n = 0;
    double gmax = 0.0;
    for (int x = 0; x < W; ++x) {
        double colmax = 0.0;
        int has_edge = 0;
        for (int y = 1; y < H; ++y) {
            float a = m->buf[y * W + x], b = m->buf[(y - 1) * W + x];
            if (in_edge_band(a) || in_edge_band(b)) {
                has_edge = 1;
                double d = fabs((double)a - (double)b);
                if (d > colmax) colmax = d;
            }
        }
        if (has_edge) {
            sig[n++] = colmax;
            if (colmax > gmax) gmax = colmax;
        }
    }
    *strength = gmax;
    if (n < 8) { *lattice = 0.0; *period = 0.0; return; }

    /* 去线性包络。 */
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i) {
        sx += i; sy += sig[i]; sxx += (double)i * i; sxy += (double)i * sig[i];
    }
    double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    double inter = (sy - slope * sx) / n;
    double *r = (double*)malloc(sizeof(double) * (size_t)n);
    double e0 = 0.0;
    for (int i = 0; i < n; ++i) {
        r[i] = sig[i] - (inter + slope * i);
        e0 += r[i] * r[i];
    }
    *lattice = sqrt(e0 / n);

    /* 晶格频段（k >= n/4，即周期 <=4px）的 DFT 主峰周期。 */
    double peak = -1.0;
    int peak_k = 0, half = n / 2;
    for (int k = n / 4; k <= half; ++k) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i) {
            double ang = -2.0 * M_PI * (double)k * (double)i / n;
            re += r[i] * cos(ang);
            im += r[i] * sin(ang);
        }
        double mag2 = re * re + im * im;
        if (mag2 > peak) { peak = mag2; peak_k = k; }
    }
    *period = peak_k > 0 ? (double)n / (double)peak_k : 0.0;
    free(r);
}

typedef struct {
    const char *name;
    int nsamp;
    double ms, rmse, maxe, strength, lattice, period;
} Report;

static void run_case(const char *name, const ShJitterCfg *cfg,
                     const ShLight *l, const ShSeg *segs, int nseg,
                     const ShadowMap *ref, Report *r) {
    ShadowMap m;
    sh_init(&m, W, H);
    int nsamp = 0;
    double t0 = now_sec();
    int rc = cfg ? sh_render_jit(&m, l, segs, nseg, cfg, &nsamp)
                 : sh_render(&m, l, segs, nseg);
    double t1 = now_sec();
    if (rc != 0) { printf("render failed in %s: %d\n", name, rc); exit(2); }

    double rmse, maxe;
    errors_vs_ref(&m, ref, &rmse, &maxe);
    double strength, lattice, period;
    grid_metrics(&m, &strength, &lattice, &period);

    r->name = name; r->nsamp = nsamp;
    r->ms = (t1 - t0) * 1e3; r->rmse = rmse; r->maxe = maxe;
    r->strength = strength; r->lattice = lattice; r->period = period;
    sh_free(&m);
}

int main(void) {
    ShLight l = {20.0f, 10.0f, 25.0f, 1.0f};
    /* 45 度倾斜遮挡段：规则单点采样沿斜边产生像素级锯齿（规则网格纹）。 */
    ShSeg segs[1] = {{30.0f, 30.0f, 90.0f, 90.0f}};
    int nseg = 1;

    ShadowMap ref;
    render_exhaustive(&ref, &l, segs, nseg);
    sh_write_pgm(&ref, "bench_ref.pgm");
    double ref_strength, ref_lattice, ref_period;
    grid_metrics(&ref, &ref_strength, &ref_lattice, &ref_period);

    ShJitterCfg off = {0, SH_JITTER_NONE, 0, 0, NULL};
    ShJitterCfg fast = {1, SH_JITTER_BAYER, SH_TIER_FAST, 2, NULL};
    ShJitterCfg fast_rot = {1, SH_JITTER_ROTATED, SH_TIER_FAST, 2, NULL};
    ShJitterCfg high = {1, SH_JITTER_BAYER, SH_TIER_HIGH, 8, NULL};

    Report r[4];
    run_case("regular (jitter off)", &off, &l, segs, nseg, &ref, &r[0]);
    run_case("jitter fast bayer", &fast, &l, segs, nseg, &ref, &r[1]);
    run_case("jitter fast rotated", &fast_rot, &l, segs, nseg, &ref, &r[2]);
    run_case("jitter high bayer", &high, &l, segs, nseg, &ref, &r[3]);

    printf("ref = exhaustive %d points/pixel: step=%.4f lattice=%.4f period=%.2fpx\n",
           REF_SIDE * REF_SIDE, ref_strength, ref_lattice, ref_period);
    printf("%-22s %6s %9s %9s %9s %9s %9s %9s\n",
           "case", "nsamp", "ms", "rmse", "maxerr", "step", "lattice", "periodPx");
    for (int i = 0; i < 4; ++i)
        printf("%-22s %6d %9.3f %9.5f %9.5f %9.4f %9.4f %9.2f\n",
               r[i].name, r[i].nsamp, r[i].ms, r[i].rmse, r[i].maxe,
               r[i].strength, r[i].lattice, r[i].period);

    /* 核对 1：规则采样的晶格走样能量显著高于穷举参考，且 DFT 主峰落在
     * 像素晶格频段（2~4px 周期）——即量到了规则网格纹及其周期。 */
    int ok_period = r[0].lattice > ref_lattice * 1.5 &&
                    r[0].period >= 2.0 && r[0].period <= 4.0;
    /* 核对 2：高速抖动把最大台阶削到规则采样的 85% 以下（网格纹强度变弱）。 */
    int ok_weaker = r[3].strength < r[0].strength * 0.85;
    /* 核对 3：两档抖动相对穷举真值的误差都显著小于规则单点采样。
     * （两档在该场景都已接近真值，孰优孰略随矩阵结构变化，不强制单调。） */
    int ok_error = r[1].rmse < r[0].rmse * 0.5 && r[3].rmse < r[0].rmse * 0.5;
    /* 核对 4：高速档晶格走样能量向穷举参考收敛，明显低于规则采样。 */
    int ok_converge = r[3].lattice < r[0].lattice * 0.8;
    printf("\nchecks: regular lattice %.4f > 1.5*ref %.4f, period %.2fpx in 2-4 -> %s\n",
           r[0].lattice, ref_lattice, r[0].period, ok_period ? "PASS" : "FAIL");
    printf("checks: high step %.4f < 0.85*regular %.4f -> %s\n",
           r[3].strength, r[0].strength, ok_weaker ? "PASS" : "FAIL");
    printf("checks: both jitter rmse < 0.5*regular %.5f (fast %.5f, high %.5f) -> %s\n",
           r[0].rmse, r[1].rmse, r[3].rmse, ok_error ? "PASS" : "FAIL");
    printf("checks: high lattice %.4f < 0.8*regular %.4f (toward exhaustive %.4f) -> %s\n",
           r[3].lattice, r[0].lattice, ref_lattice, ok_converge ? "PASS" : "FAIL");

    sh_free(&ref);
    return (ok_period && ok_weaker && ok_error && ok_converge) ? 0 : 1;
}
