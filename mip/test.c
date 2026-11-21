#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

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

    /* 新路径：纹素中心 uv=(i+0.5)/W 必须采到纹素 i 本身 */
    MipDiag d;
    unsigned char a[3], b[3];
    mip_diag_init(&d, 1);
    double uc = (0.0 + 0.5) / W, vc = (3.0 + 0.5) / W;
    CK(mip_sample_ex(&c, 0, uc, vc, a, &d) == 0);
    CK(mip_sample_ex(&c, 0, uc, vc, b, NULL) == 0);
    CK(memcmp(a, b, 3) == 0);
    CK(d.last_raw_x == 0 && d.last_raw_y == 3);
    CK(d.last_oob == 0);

    /* u=0 时 raw=floor(-0.5)=-1，越界检出并以安全纹素填充，返回 1 */
    CK(mip_sample_ex(&c, 0, 0.0, 0.0, a, &d) == 1);
    CK(d.last_oob == 1 && d.last_raw_x == -1 && d.last_raw_y == -1);
    CK(d.oob == 1);

    /* 老路径在 u=0 取纹素 0，新路径 raw=-1 被 clamp 到 0：该样本计越界不计修正 */
    CK(d.corrections == 0);

    /* 边界 u=1 -> raw=W-1，不越界；负小数 floor 语义检查 */
    CK(mip_sample_ex(&c, 0, 1.0, 1.0, a, &d) == 0);
    CK(d.last_raw_x == W - 1 && d.last_raw_y == H - 1);
    CK((int)floor(-0.5) == -1);

    /* 修正计数：u=1/W 处老路径 x=1，新路径 raw=0，raw 合法且不同 -> 计一次修正 */
    int corr_before = d.corrections;
    CK(mip_sample_ex(&c, 0, 1.0 / W, 1.0 / H, a, &d) == 0);
    CK(d.last_raw_x == 0 && d.last_old_x == 1);
    CK(d.corrections == corr_before + 2);

    /* 关闭新逻辑：与 mip_sample 字节一致，且不累计修正/越界 */
    MipDiag off;
    mip_diag_init(&off, 0);
    int mismatch = 0;
    double probes[] = {0.0, 0.5 / W, 1.0 / W, 0.5, 1.0, -0.2, 1.3};
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i)
        for (size_t j = 0; j < sizeof(probes)/sizeof(probes[0]); ++j) {
            unsigned char p1[3], p2[3];
            int r1 = mip_sample_ex(&c, 0, probes[i], probes[j], p1, &off);
            int r2 = mip_sample(&c, 0, probes[i], probes[j], p2);
            if (r1 != r2 || memcmp(p1, p2, 3) != 0) ++mismatch;
        }
    CK(mismatch == 0);
    CK(off.samples == (int)(sizeof(probes)/sizeof(probes[0])) * (int)(sizeof(probes)/sizeof(probes[0])));
    CK(off.corrections == 0 && off.oob == 0);

    /* 参数错误 */
    CK(mip_sample_ex(&c, 9, 0.5, 0.5, px, &d) == -1);
    CK(mip_sample_ex(NULL, 0, 0.5, 0.5, px, &d) == -1);

    mip_free(&c);
    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
