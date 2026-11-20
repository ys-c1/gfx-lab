#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define PI 3.14159265358979323846

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static double sinc1(double t) { return t == 0.0 ? 1.0 : sin(t) / t; }

/* 正弦纹理 f = 0.5 + 0.5*sin(2πfu u)*sin(2πfv v) 在足迹平行四边形上的解析平均 */
static double analytic_avg(double u0, double v0,
                           double dudx, double dvdx, double dudy, double dvdy,
                           double fu, double fv) {
    double au = 2*PI*fu*dudx, bu = 2*PI*fu*dudy, cu = 2*PI*fu*u0;
    double av = 2*PI*fv*dvdx, bv = 2*PI*fv*dvdy, cv = 2*PI*fv*v0;
    double m = cos(cu-cv)*sinc1(0.5*(au-av))*sinc1(0.5*(bu-bv))
             - cos(cu+cv)*sinc1(0.5*(au+av))*sinc1(0.5*(bu+bv));
    return 0.5 + 0.25*m;
}

int main(void) {
    enum { W = 16, H = 16 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int i = 0; i < W*H; ++i) { src[i*3+0] = (unsigned char)(i % 256); src[i*3+1] = 40; src[i*3+2] = 200; }
    MipChain c;
    CK(mip_build(&c, src, W, H) == 0);
    CK(c.levels == 5);
    CK(c.w[1] == 8 && c.h[1] == 8);
    unsigned char px[3];
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == MIP_ERR_LEVEL);
    CK(mip_sample(&c, -1, 0.5, 0.5, px) == MIP_ERR_LEVEL);
    CK(mip_sample(&c, 0, NAN, 0.5, px) == MIP_ERR_UV);

    /* 关闭（max_aniso<=1）时输出与 mip_sample 单次采样完全相同 */
    MipAnisoInfo inf;
    unsigned char a[3], b[3];
    double dudx = 4.0/W, dvdx = 1.0/H, dudy = 0.5/W, dvdy = 2.0/H;
    CK(mip_sample_aniso(&c, 0.3, 0.7, dudx, dvdx, dudy, dvdy, 1, a, &inf) == MIP_OK);
    CK(inf.nsamples == 1);
    CK(mip_sample(&c, inf.level, 0.3, 0.7, b) == MIP_OK);
    CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
    CK(mip_sample_aniso(&c, 0.3, 0.7, dudx, dvdx, dudy, dvdy, 0, a, &inf) == MIP_OK
       && inf.nsamples == 1);

    /* 非有限导数逐参数拒绝，错误码指出参数 */
    CK(mip_sample_aniso(&c, 0.3, 0.7, NAN, dvdx, dudy, dvdy, 4, a, 0) == MIP_ERR_DUDX);
    CK(mip_sample_aniso(&c, 0.3, 0.7, dudx, INFINITY, dudy, dvdy, 4, a, 0) == MIP_ERR_DVDX);
    CK(mip_sample_aniso(&c, 0.3, 0.7, dudx, dvdx, NAN, dvdy, 4, a, 0) == MIP_ERR_DUDY);
    CK(mip_sample_aniso(&c, 0.3, 0.7, dudx, dvdx, dudy, -INFINITY, 4, a, 0) == MIP_ERR_DVDY);
    CK(mip_sample_aniso(&c, 0.3, INFINITY, dudx, dvdx, dudy, dvdy, 4, a, 0) == MIP_ERR_UV);
    CK(mip_strerror(MIP_ERR_DUDX) != 0 && mip_strerror(MIP_ERR_LEVEL) != 0);

    /* 长宽比决定子样本数；请求倍率再大也封顶 MIP_ANISO_CAP */
    CK(mip_sample_aniso(&c, 0.5, 0.5, 48.0/W, 0, 0, 1.0/H, 4, a, &inf) == MIP_OK
       && inf.nsamples == 4);
    CK(mip_sample_aniso(&c, 0.5, 0.5, 48.0/W, 0, 0, 1.0/H, 16, a, &inf) == MIP_OK
       && inf.nsamples == 16);
    CK(mip_sample_aniso(&c, 0.5, 0.5, 48.0/W, 0, 0, 1.0/H, 1000, a, &inf) == MIP_OK
       && inf.nsamples == MIP_ANISO_CAP);
    CK(mip_sample_aniso(&c, 0.5, 0.5, 3.0/W, 0, 0, 1.0/H, 16, a, &inf) == MIP_OK
       && inf.nsamples == 3);
    mip_free(&c);
    free(src);

    /* 斜向长足迹：16x 比关闭更接近解析积分 */
    {
        enum { TW = 64, TH = 64 };
        unsigned char *tex = (unsigned char*)malloc(TW*TH*3);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x) {
                double uu = (x + 0.5) / TW, vv = (y + 0.5) / TH;
                double s = 0.5 + 0.5 * sin(2*PI*6*uu) * sin(2*PI*6*vv);
                unsigned char q = (unsigned char)(s * 255.0 + 0.5);
                tex[(y*TW+x)*3+0] = q; tex[(y*TW+x)*3+1] = q; tex[(y*TW+x)*3+2] = q;
            }
        MipChain t;
        CK(mip_build(&t, tex, TW, TH) == 0);
        double du = 12.0/TW, dv = 1.0/TH;   /* 12:1 足迹 */
        double e1 = 0, e16 = 0;
        for (int i = 0; i < 64; ++i) {
            double u0 = 0.12 + 0.76 * i / 63.0, v0 = 0.5;
            double exact = analytic_avg(u0, v0, du, 0, 0, dv, 6.0, 6.0);
            unsigned char o1[3], o16[3];
            CK(mip_sample_aniso(&t, u0, v0, du, 0, 0, dv, 1, o1, 0) == MIP_OK);
            CK(mip_sample_aniso(&t, u0, v0, du, 0, 0, dv, 16, o16, 0) == MIP_OK);
            double g1 = o1[0] / 255.0, g16 = o16[0] / 255.0;
            e1 += (g1-exact)*(g1-exact);
            e16 += (g16-exact)*(g16-exact);
        }
        CK(e16 < e1);
        mip_free(&t);
        free(tex);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
