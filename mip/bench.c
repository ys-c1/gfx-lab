#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define PI 3.14159265358979323846

enum { TW = 256, TH = 256, NPIX = 4096, REPS = 100 };

/* 逐通道频率的正弦纹理：可解析积分，作为清晰度对照基准 */
static const double FU[3] = { 6.0, 11.0, 23.0 };
static const double FV[3] = { 5.0,  9.0, 17.0 };

static double sinc1(double t) { return t == 0.0 ? 1.0 : sin(t) / t; }

/* f = 0.5 + 0.5*sin(2πfu u)*sin(2πfv v) 在足迹平行四边形上的解析平均：
 * 乘积化和差后各项为屏幕坐标线性函数的余弦，单位正方形上可分离积分 */
static double analytic_avg(double u0, double v0,
                           double dudx, double dvdx, double dudy, double dvdy,
                           double fu, double fv) {
    double au = 2*PI*fu*dudx, bu = 2*PI*fu*dudy, cu = 2*PI*fu*u0;
    double av = 2*PI*fv*dvdx, bv = 2*PI*fv*dvdy, cv = 2*PI*fv*v0;
    double m = cos(cu-cv)*sinc1(0.5*(au-av))*sinc1(0.5*(bu-bv))
             - cos(cu+cv)*sinc1(0.5*(au+av))*sinc1(0.5*(bu+bv));
    return 0.5 + 0.25*m;
}

static double now_ms(void) {
    return 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(void) {
    unsigned char *tex = (unsigned char*)malloc((size_t)TW*TH*3);
    if (!tex) return 1;
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            double uu = (x + 0.5) / TW, vv = (y + 0.5) / TH;
            for (int ch = 0; ch < 3; ++ch) {
                double s = 0.5 + 0.5 * sin(2*PI*FU[ch]*uu) * sin(2*PI*FV[ch]*vv);
                tex[(y*TW+x)*3+ch] = (unsigned char)(s * 255.0 + 0.5);
            }
        }
    MipChain c;
    if (mip_build(&c, tex, TW, TH) != 0) return 1;

    /* 斜视足迹：短轴 1→3 纹素，长宽比 1→32，长轴方向 25° */
    double phi = 25.0 * PI / 180.0;
    double *pu = malloc(sizeof(double)*NPIX), *pv = malloc(sizeof(double)*NPIX);
    double *pdudx = malloc(sizeof(double)*NPIX), *pdvdx = malloc(sizeof(double)*NPIX);
    double *pdudy = malloc(sizeof(double)*NPIX), *pdvdy = malloc(sizeof(double)*NPIX);
    double *exact = malloc(sizeof(double)*NPIX*3);
    if (!pu || !pv || !pdudx || !pdvdx || !pdudy || !pdvdy || !exact) return 1;
    for (int i = 0; i < NPIX; ++i) {
        double t = (double)i / (NPIX - 1);
        double minor = 1.0 + 2.0 * t;            /* 纹素 */
        double major = minor * (1.0 + 31.0 * t); /* 长宽比 1→32 */
        unsigned s0 = (unsigned)i * 2654435761u, s1 = (unsigned)i * 2246822519u + 7u;
        double u0 = 0.2 + 0.6 * (s0 & 0xFFFF) / 65535.0;
        double v0 = 0.2 + 0.6 * (s1 & 0xFFFF) / 65535.0;
        double dudx = -major * sin(phi) / TW, dvdx = major * cos(phi) / TH;
        double dudy =  minor * cos(phi) / TW, dvdy = minor * sin(phi) / TH;
        pu[i] = u0; pv[i] = v0;
        pdudx[i] = dudx; pdvdx[i] = dvdx; pdudy[i] = dudy; pdvdy[i] = dvdy;
        for (int ch = 0; ch < 3; ++ch)
            exact[i*3+ch] = analytic_avg(u0, v0, dudx, dvdx, dudy, dvdy, FU[ch], FV[ch]);
    }

    static const struct { const char *name; int max_aniso; } settings[] = {
        { "off (1x)",          1 },
        { "aniso 4x",          4 },
        { "aniso 16x",        16 },
        { "aniso 1024x(cap)", 1024 },
    };
    printf("mip aniso bench: %dx%d texture, %d oblique pixels (ratio 1->32, minor 1->3 texels)\n",
           TW, TH, NPIX);
    printf("error = RMSE vs analytic footprint average, 0-255 scale; time = CPU ms per sweep\n");
    printf("%-18s %10s %12s %12s %10s\n", "setting", "rmse", "samples", "samples/px", "ms");
    for (size_t s = 0; s < sizeof(settings)/sizeof(settings[0]); ++s) {
        double se = 0.0, t0 = now_ms();
        long samples = 0;
        for (int rep = 0; rep < REPS; ++rep)
            for (int i = 0; i < NPIX; ++i) {
                unsigned char o[3];
                MipAnisoInfo inf;
                if (mip_sample_aniso(&c, pu[i], pv[i], pdudx[i], pdvdx[i], pdudy[i], pdvdy[i],
                                     settings[s].max_aniso, o, &inf) != MIP_OK) return 1;
                if (rep == 0) {
                    samples += inf.nsamples;
                    for (int ch = 0; ch < 3; ++ch) {
                        double d = o[ch] / 255.0 - exact[i*3+ch];
                        se += d * d;
                    }
                }
            }
        double ms = (now_ms() - t0) / REPS;
        double rmse = sqrt(se / ((double)NPIX * 3)) * 255.0;
        printf("%-18s %10.3f %12ld %12.2f %10.2f\n",
               settings[s].name, rmse, samples, (double)samples / NPIX, ms);
    }
    printf("analytic reference: rmse 0 by definition (exact footprint integral)\n");

    free(pu); free(pv); free(pdudx); free(pdvdx); free(pdudy); free(pdvdy); free(exact);
    mip_free(&c);
    free(tex);
    return 0;
}
