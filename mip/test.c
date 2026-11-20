#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define GAMMA 2.2

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
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == -1);

    /* 不转空间:mip_sample_ex 与 mip_sample 逐字节一致 */
    for (int l = 0; l < c.levels; ++l) {
        unsigned char a[3], b[3];
        CK(mip_sample(&c, l, 0.3, 0.7, a) == 0);
        CK(mip_sample_ex(&c, (double)l, 0.3, 0.7, b, NULL, NULL) == MIP_OK);
        CK(memcmp(a, b, 3) == 0);
    }
    mip_free(&c);

    /* 报错:gamma 非法 / 层数为零 / 需求层号越界,字段名要对 */
    MipError e;
    CK(mip_build_ex(&c, src, W, H, -1.0, &e) == MIP_ERR_GAMMA);
    CK(e.field && strcmp(e.field, "gamma") == 0);
    CK(mip_build_ex(&c, src, W, H, NAN, &e) == MIP_ERR_GAMMA);
    CK(e.field && strcmp(e.field, "gamma") == 0);
    MipChain z;
    memset(&z, 0, sizeof z);
    CK(mip_sample_ex(&z, 0.0, 0.5, 0.5, px, NULL, &e) == MIP_ERR_LEVELS);
    CK(e.field && strcmp(e.field, "levels") == 0);
    CK(mip_build_ex(&c, src, W, H, GAMMA, &e) == MIP_OK);
    CK(mip_sample_ex(&c, 5.0, 0.5, 0.5, px, NULL, &e) == MIP_ERR_LEVEL);
    CK(e.field && strcmp(e.field, "level") == 0);
    CK(mip_sample_ex(&c, -0.5, 0.5, 0.5, px, NULL, &e) == MIP_ERR_LEVEL);
    CK(e.field && strcmp(e.field, "level") == 0);
    mip_free(&c);

    /* 手算对照:1px 棋盘格 0/255,线性平均 0.5 -> 编回 0.5^(1/2.2)*255 = 186 */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char v = (unsigned char)(((x ^ y) & 1) ? 255 : 0);
            src[(y*W+x)*3+0] = v; src[(y*W+x)*3+1] = v; src[(y*W+x)*3+2] = v;
        }
    int expect186 = (int)(pow(0.5, 1.0 / GAMMA) * 255.0 + 0.5); /* 186 */
    CK(expect186 == 186);
    CK(mip_build_ex(&c, src, W, H, GAMMA, NULL) == MIP_OK);
    CK(mip_sample_ex(&c, 4.0, 0.5, 0.5, px, NULL, NULL) == MIP_OK);
    CK(px[0] == expect186 && px[1] == expect186 && px[2] == expect186);
    /* 旧链同一点仍是编码空间平均 127,即"亮部发暗" */
    MipChain leg;
    CK(mip_build(&leg, src, W, H) == 0);
    CK(mip_sample(&leg, 4, 0.5, 0.5, px) == 0);
    CK(px[0] == 127);
    mip_free(&leg);
    /* 小数层号:黑格上 level 0.5 -> 线性 0.25 -> 编回 0.25^(1/2.2)*255 = 136 */
    MipTrace tr;
    CK(mip_sample_ex(&c, 0.5, 0.01, 0.01, px, &tr, NULL) == MIP_OK);
    CK(tr.lo == 0 && tr.hi == 1 && tr.t == 0.5 && tr.samples == 2);
    int expect136 = (int)(pow(0.25, 1.0 / GAMMA) * 255.0 + 0.5); /* 136 */
    CK(px[0] == expect136);
    /* 整数层号只取一层 */
    CK(mip_sample_ex(&c, 2.0, 0.5, 0.5, px, &tr, NULL) == MIP_OK);
    CK(tr.lo == 2 && tr.hi == 2 && tr.samples == 1);
    mip_free(&c);

    /* 常量图往返:任意层号编回后仍是原值 */
    for (int i = 0; i < W*H*3; ++i) src[i] = 100;
    CK(mip_build_ex(&c, src, W, H, GAMMA, NULL) == MIP_OK);
    for (double l = 0.0; l <= 4.0; l += 1.0) {
        CK(mip_sample_ex(&c, l, 0.4, 0.6, px, NULL, NULL) == MIP_OK);
        CK(abs(px[0] - 100) <= 1);
    }
    mip_free(&c);

    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
