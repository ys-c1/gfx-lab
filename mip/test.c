#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 每个采样器持有独立的寻址表，避免共享存储被后续用例污染。 */
#define DEF_SAMPLER(name, en, mu, mv) \
    MipAddr name##_table[2] = { { mu }, { mv } }; \
    MipSampler name = { en, name##_table }

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

    /* 关闭寻址：mip_sample_ex 必须与旧 mip_sample 逐像素相同（含越界坐标）。 */
    DEF_SAMPLER(off, 0, MIP_ADDR_WRAP, MIP_ADDR_WRAP);
    double probe[][2] = { {0.5, 0.5}, {-0.7, 1.3}, {2.25, -3.5}, {12.0, 0.0}, {0.0, 0.999999} };
    for (unsigned i = 0; i < sizeof(probe)/sizeof(probe[0]); ++i) {
        unsigned char a[3], b[3], d[3];
        CK(mip_sample(&c, 0, probe[i][0], probe[i][1], a) == 0);
        CK(mip_sample_ex(&c, 0, &off, probe[i][0], probe[i][1], 7, b) == 0);
        CK(mip_sample_ex(&c, 0, NULL, probe[i][0], probe[i][1], 7, d) == 0);
        CK(a[0]==b[0] && a[1]==b[1] && a[2]==b[2]);
        CK(a[0]==d[0] && a[1]==d[1] && a[2]==d[2]);
    }

    /* wrap：负坐标回绕、正坐标取小数部分。 */
    DEF_SAMPLER(wp, 1, MIP_ADDR_WRAP, MIP_ADDR_CLAMP);
    double tu;
    CK(mip_address(&wp, 0, -0.25, 1, &tu) == 0 && fabs(tu - 0.75) < 1e-12);
    CK(mip_address(&wp, 0, 1.25, 1, &tu) == 0 && fabs(tu - 0.25) < 1e-12);
    CK(mip_address(&wp, 0, 0.0, 1, &tu) == 0 && tu == 0.0);

    /* mirror：整数段奇偶交替。 */
    DEF_SAMPLER(mr, 1, MIP_ADDR_MIRROR, MIP_ADDR_MIRROR);
    CK(mip_address(&mr, 0, 0.3, 1, &tu) == 0 && fabs(tu - 0.3) < 1e-12);
    CK(mip_address(&mr, 0, 1.3, 1, &tu) == 0 && fabs(tu - 0.7) < 1e-12);
    CK(mip_address(&mr, 0, 2.3, 1, &tu) == 0 && fabs(tu - 0.3) < 1e-12);
    CK(mip_address(&mr, 0, -1.3, 1, &tu) == 0 && fabs(tu - 0.7) < 1e-12);
    /* 精确整数点（含奇数段折返点）必须严格落在 [0,1)，采样不得越界。 */
    double intpts[] = {0.0, 1.0, -1.0, 2.0, 3.0, -3.0};
    for (unsigned i = 0; i < sizeof(intpts)/sizeof(intpts[0]); ++i) {
        CK(mip_address(&mr, 0, intpts[i], 9, &tu) == 0);
        CK(tu >= 0.0 && tu < 1.0);
        unsigned char q[3];
        CK(mip_sample_ex(&c, 0, &mr, intpts[i], intpts[i], 9, q) == 0);
    }

    /* clamp 寻址与关闭寻址的夹边结果在任意坐标下一致。 */
    DEF_SAMPLER(cl, 1, MIP_ADDR_CLAMP, MIP_ADDR_CLAMP);
    for (unsigned i = 0; i < sizeof(probe)/sizeof(probe[0]); ++i) {
        unsigned char a[3], b[3];
        CK(mip_sample(&c, 0, probe[i][0], probe[i][1], a) == 0);
        CK(mip_sample_ex(&c, 0, &cl, probe[i][0], probe[i][1], 3, b) == 0);
        CK(a[0]==b[0] && a[1]==b[1] && a[2]==b[2]);
    }
    CK(mip_sample_ex(&c, 0, &wp, 0.0, 0.0, 1, px) == 0);

    /* 错误路径：模式非法、非有限坐标、寻址表缺失，均带三角形 id 报错。 */
    DEF_SAMPLER(bad_mode, 1, 99, MIP_ADDR_WRAP);
    CK(mip_sample_ex(&c, 0, &bad_mode, 0.5, 0.5, 42, px) == MIP_ERR_MODE);
    CK(mip_address(&bad_mode, 0, 0.5, 42, &tu) == MIP_ERR_MODE);

    double infv = INFINITY, nanv = NAN;
    CK(mip_sample_ex(&c, 0, &wp, infv, 0.5, 43, px) == MIP_ERR_NONFIN);
    CK(mip_sample_ex(&c, 0, &wp, 0.5, nanv, 44, px) == MIP_ERR_NONFIN);
    CK(mip_address(&wp, 1, -infv, 45, &tu) == MIP_ERR_NONFIN);

    MipSampler no_table;
    no_table.enabled = 1; no_table.addr = NULL;
    CK(mip_sample_ex(&c, 0, &no_table, 0.5, 0.5, 46, px) == MIP_ERR_NOTABLE);
    CK(mip_address(&no_table, 0, 0.5, 46, &tu) == MIP_ERR_NOTABLE);

    /* 关闭寻址时非有限坐标不报错（旧路径行为：夹边）。 */
    CK(mip_sample_ex(&c, 0, &off, infv, 0.5, 1, px) == 0);
    /* 关闭寻址时寻址表缺失也不报错：不经过寻址表。 */
    MipSampler off_no_table;
    off_no_table.enabled = 0; off_no_table.addr = NULL;
    CK(mip_sample_ex(&c, 0, &off_no_table, 0.5, 0.5, 1, px) == 0);
    CK(mip_address(&off_no_table, 0, 0.5, 1, &tu) == 0 && tu == 0.5);

    mip_free(&c);
    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
