#include "tex.h"
#include "tex_ref.h"
#include "atlas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 可预测的合成纹理：棋盘 + 梯度，含高频以便观察各层差异 */
static void fill_tex(unsigned char *buf, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *p = buf + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)(((x / 2) + (y / 2)) % 2 ? 230 : 20);
            p[1] = (unsigned char)((x * 7 + y * 3) % 256);
            p[2] = (unsigned char)((x * 5) % 256);
        }
}

/* 把若干图块按货架结果拷进图集位图 */
static void blit(unsigned char *atlas, int aw,
                 const unsigned char *src, int sw, int sh, int ox, int oy) {
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            for (int ch = 0; ch < 3; ++ch)
                atlas[((size_t)(oy + y) * aw + (ox + x)) * 3 + ch] =
                    src[((size_t)y * sw + x) * 3 + ch];
}

static double frand(void) { return (double)rand() / (double)RAND_MAX; }

/* 测试错误路径时静音/恢复 stderr（dup 保存原 fd，不依赖 /dev/tty） */
static int saved_err = -1;
static void hush(int on) {
    if (on) {
        fflush(stderr);
        saved_err = dup(STDERR_FILENO);
        FILE *f = freopen("/dev/null", "w", stderr);
        (void)f;
    } else if (saved_err >= 0) {
        fflush(stderr);
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
        saved_err = -1;
    }
}

int main(void) {
    srand(1234);

    /* ---- 构建 2 个 32x32 图块，货架打包进 64 宽图集 ---- */
    enum { TILE = 32, AW = 64, AH = 32 };
    unsigned char *tiles = (unsigned char*)malloc((size_t)TILE * TILE * 3 * 2);
    fill_tex(tiles, TILE, TILE);
    fill_tex(tiles + (size_t)TILE * TILE * 3, TILE, TILE);
    AtlasRect rects[2] = {{TILE, TILE}, {TILE, TILE}};
    AtlasPos pos[2];
    AtlasStats stats;
    CK(atlas_pack(rects, 2, AW, pos, &stats) == 0);
    unsigned char *atlas = (unsigned char*)calloc((size_t)AW * AH * 3, 1);
    blit(atlas, AW, tiles, TILE, TILE, pos[0].x, pos[0].y);
    blit(atlas, AW, tiles + (size_t)TILE * TILE * 3, TILE, TILE, pos[1].x, pos[1].y);

    MipChain chain, idchain;
    CK(mip_build(&chain, atlas, AW, AH) == 0);
    /* 恒等链：图块即整张图，用于验证 legacy 与 mip_sample 字节一致 */
    CK(mip_build(&idchain, tiles, TILE, TILE) == 0);

    unsigned char a[3], b[3];

    /* 1. 关闭偏置 + 恒等图块：逐像素与 mip_sample 完全相同 */
    for (int lvl = 0; lvl < idchain.levels; ++lvl)
        for (int i = 0; i < 200; ++i) {
            double u = frand(), v = frand();
            TexReq r = {0};
            r.chain = &idchain;
            r.atlas_w = idchain.w[0]; r.atlas_h = idchain.h[0];
            r.tile_x = 0; r.tile_y = 0;
            r.tile_w = idchain.w[0]; r.tile_h = idchain.h[0];
            r.bias_enabled = 0; r.level = lvl; r.u = u; r.v = v;
            CK(tex_sample(&r, a) == 0);
            CK(mip_sample(&idchain, lvl, u, v, b) == 0);
            CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
            CK(r.taps == 1);
        }

    /* 2. 关闭偏置 + 非恒等图块：重映射后与 mip_sample 点采样一致 */
    for (int i = 0; i < 200; ++i) {
        double u = frand(), v = frand();
        int lvl = rand() % chain.levels;
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_x = pos[1].x; r.tile_y = pos[1].y; r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 0; r.level = lvl; r.u = u; r.v = v;
        CK(tex_sample(&r, a) == 0);
        double au = (pos[1].x + u * TILE) / (double)AW;
        double av = (pos[1].y + v * TILE) / (double)AH;
        CK(mip_sample(&chain, lvl, au, av, b) == 0);
        CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
    }

    /* 3. 错误：偏置非有限，必须报字段名 'bias' */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 1; r.bias = NAN; r.mode = TEX_TRILINEAR;
        r.u = 0.3; r.v = 0.3;
        hush(1);
        CK(tex_sample(&r, a) == -3);
        hush(0);
        r.bias = INFINITY;
        hush(1);
        CK(tex_sample(&r, a) == -3);
        hush(0);
    }

    /* 4. 错误：导数非有限，字段名定位到具体导数 */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
        r.u = 0.3; r.v = 0.3; r.dudx = NAN;
        hush(1);
        CK(tex_sample(&r, a) == -3);
        hush(0);
    }

    /* 5. 错误：legacy 层号越界 -> 'level' */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 0; r.level = chain.levels; r.u = 0.1; r.v = 0.1;
        hush(1);
        CK(tex_sample(&r, a) == -4);
        hush(0);
        r.level = -1;
        hush(1);
        CK(tex_sample(&r, a) == -4);
        hush(0);
    }

    /* 6. 错误：导数模式层号越界 -> 'lod'（巨大导数把 LOD 顶出顶层） */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
        r.u = 0.5; r.v = 0.5; r.dudx = 1e6; r.dvdy = 1e6;
        hush(1);
        CK(tex_sample(&r, a) == -4);
        hush(0);
    }

    /* 7. 错误：图块越界，逐字段命名 */
    {
        struct { int x, y, w, h; } badcases[4] = {
            {-1, 0, TILE, TILE}, {0, 0, AW + 1, TILE},
            {0, 0, TILE, AH + 1}, {AW - TILE + 1, 0, TILE, TILE}
        };
        for (int bi = 0; bi < 4; ++bi) {
            TexReq r = {0};
            r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
            r.tile_x = badcases[bi].x; r.tile_y = badcases[bi].y;
            r.tile_w = badcases[bi].w; r.tile_h = badcases[bi].h;
            r.bias_enabled = 0; r.level = 0; r.u = 0.1; r.v = 0.1;
            hush(1);
            CK(tex_sample(&r, a) == -2);
            hush(0);
        }
    }

    /* 8. 错误：空指针 / 非法 mode */
    {
        TexReq r = {0};
        r.chain = NULL; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 0; r.level = 0; r.u = 0.1; r.v = 0.1;
        hush(1);
        CK(tex_sample(&r, a) == -1);
        hush(0);

        TexReq r2 = {0};
        r2.chain = &chain; r2.atlas_w = AW; r2.atlas_h = AH;
        r2.tile_w = TILE; r2.tile_h = TILE;
        r2.bias_enabled = 1; r2.bias = 0.0; r2.mode = (TexMode)99;
        r2.u = 0.3; r2.v = 0.3;
        hush(1);
        CK(tex_sample(&r2, a) == -5);
        hush(0);
    }

    /* 9. taps：底层三线性只采 4；顶层三线性只采 4；中间层 8 */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
        r.u = 0.5; r.v = 0.5;
        /* rho=1 -> lambda=0 -> 底层，t=0，只采 4 */
        r.dudx = 1.0 / TILE; r.dvdy = 1.0 / TILE;
        CK(tex_sample(&r, a) == 0 && r.taps == 4);
        /* rho=3 -> lambda=1.585，非整数权重，相邻两层都采，8 taps */
        r.dudx = 3.0 / TILE; r.dvdy = 3.0 / TILE;
        CK(tex_sample(&r, a) == 0 && r.taps == 8);
    }

    /* 10. 各向异性：长宽比越大子样本越多，taps 随 N 增长，且 N 受上限钳制 */
    {
        TexReq r = {0};
        r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
        r.tile_w = TILE; r.tile_h = TILE;
        r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_ANISO;
        r.u = 0.5; r.v = 0.5;
        long prev = 0;
        for (int k = 1; k <= 8; k *= 2) {
            r.dudx = (double)k / TILE; r.dvdy = 1.0 / TILE;
            CK(tex_sample(&r, a) == 0);
            CK(r.taps >= prev);
            prev = r.taps;
        }
        /* 长宽比超过 TEX_MAX_ANISO：N 被钳到 8，两次调用 taps 相同 */
        r.dudx = 32.0 / TILE; r.dvdy = 1.0 / TILE; /* 32:1，N 钳到 8 */
        CK(tex_sample(&r, a) == 0);
        long capped = r.taps;
        r.dudx = 16.0 / TILE; /* 16:1，N 同样钳到 8，足迹仍在链内 */
        CK(tex_sample(&r, a) == 0 && r.taps == capped);
    }

    /* 11a. 与自建参考采样器严格对账（线性梯度）：
     *   - 放大（足迹<=1 texel）：两条路径同一双线性约定，必须逐字节一致；
     *   - 对齐的 2 的幂缩小：mip 纹素即足迹盒的精确平均，必须逐字节一致；
     *   - 任意大足迹：mip 网格与足迹盒有半 texel 级对齐差，只验有界。 */
    {
        enum { GW = 16, GH = 16 };
        unsigned char *grad = (unsigned char*)malloc((size_t)GW * GH * 3);
        for (int y = 0; y < GH; ++y)
            for (int x = 0; x < GW; ++x) {
                unsigned char *p = grad + (y * GW + x) * 3;
                p[0] = (unsigned char)(x * 8); p[1] = (unsigned char)(y * 8); p[2] = 128;
            }
        MipChain gc;
        CK(mip_build(&gc, grad, GW, GH) == 0);

        /* 严格不变量 1：放大逐字节一致 */
        for (int k = 0; k < 500; ++k) {
            double u = frand(), v = frand();
            double mag = 0.2 + 0.8 * frand(); /* 0.2..1.0 texel 足迹 */
            TexReq r = {0};
            r.chain = &gc; r.atlas_w = GW; r.atlas_h = GH;
            r.tile_w = GW; r.tile_h = GH;
            r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
            r.u = u; r.v = v; r.dudx = mag / GW; r.dvdy = mag / GH;
            CK(tex_sample(&r, a) == 0);
            CK(tex_ref_sample(&gc, GW, GH, 0, 0, GW, GH,
                              u, v, r.dudx, 0.0, 0.0, r.dvdy, b) == 0);
            CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
        }

        /* 严格不变量 2：对齐的 2 的幂缩小逐字节一致 */
        for (int mag = 2; mag <= 8; mag *= 2)
            for (int ci = 1; ci < GW / mag; ++ci) {
                double u = ((double)ci * mag + mag / 2.0) / GW;
                double v = 0.5;
                TexReq r = {0};
                r.chain = &gc; r.atlas_w = GW; r.atlas_h = GH;
                r.tile_w = GW; r.tile_h = GH;
                r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
                r.u = u; r.v = v; r.dudx = (double)mag / GW; r.dvdy = (double)mag / GH;
                CK(tex_sample(&r, a) == 0);
                CK(tex_ref_sample(&gc, GW, GH, 0, 0, GW, GH,
                                  u, v, r.dudx, 0.0, 0.0, r.dvdy, b) == 0);
                CK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
            }

        /* 有界性：任意足迹随机采样，RMSE 与单点偏差受限 */
        double sq_sum = 0.0; int n = 0, max_abs = 0;
        for (int k = 0; k < 400; ++k) {
            double u = 0.05 + 0.9 * frand(), v = 0.05 + 0.9 * frand();
            double mag = pow(8.0, frand());
            TexReq r = {0};
            r.chain = &gc; r.atlas_w = GW; r.atlas_h = GH;
            r.tile_w = GW; r.tile_h = GH;
            r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
            r.u = u; r.v = v; r.dudx = mag / GW; r.dvdy = mag / GH;
            CK(tex_sample(&r, a) == 0);
            CK(tex_ref_sample(&gc, GW, GH, 0, 0, GW, GH,
                              u, v, r.dudx, 0.0, 0.0, r.dvdy, b) == 0);
            for (int ch = 0; ch < 3; ++ch) {
                int d = (int)a[ch] - (int)b[ch];
                sq_sum += (double)(d * d);
                if (abs(d) > max_abs) max_abs = abs(d);
                ++n;
            }
        }
        double rmse = sqrt(sq_sum / (double)n);
        printf("reference reconciliation (linear gradient): RMSE=%.3f max|d|=%d over %d channels\n",
               rmse, max_abs, n);
        CK(rmse < 3.0);
        CK(max_abs <= 16);
        mip_free(&gc);
        free(grad);
    }

    /* 11b. 高频棋盘：盒式金字塔与任意位置足迹盒存在相位差，只验整体收敛与有界 */
    {
        double sq_sum = 0.0; int n = 0, max_abs = 0;
        for (int k = 0; k < 500; ++k) {
            double u = 0.05 + 0.9 * frand(), v = 0.05 + 0.9 * frand();
            double mag = pow(8.0, frand()); /* 足迹 1x..8x texel */
            TexReq r = {0};
            r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
            r.tile_x = pos[0].x; r.tile_y = pos[0].y; r.tile_w = TILE; r.tile_h = TILE;
            r.bias_enabled = 1; r.bias = 0.0; r.mode = TEX_TRILINEAR;
            r.u = u; r.v = v;
            r.dudx = mag / TILE; r.dvdy = mag / TILE;
            CK(tex_sample(&r, a) == 0);
            CK(tex_ref_sample(&chain, AW, AH, pos[0].x, pos[0].y, TILE, TILE,
                              u, v, r.dudx, r.dudy, r.dvdx, r.dvdy, b) == 0);
            for (int ch = 0; ch < 3; ++ch) {
                int d = (int)a[ch] - (int)b[ch];
                sq_sum += (double)(d * d);
                if (abs(d) > max_abs) max_abs = abs(d);
                ++n;
            }
        }
        double rmse = sqrt(sq_sum / (double)n);
        printf("reference reconciliation (checkerboard): RMSE=%.3f max|d|=%d over %d channels\n",
               rmse, max_abs, n);
        CK(rmse < 12.0);   /* 盒式金字塔近似盒式面积平均，误差有界 */
        CK(max_abs <= 64); /* 2px 棋盘上的相位差，单点允许较大偏差 */
    }

    /* 12. 各向异性比对：拉长足迹下 aniso 的对账误差应不劣于 trilinear */
    {
        double sq_tri = 0.0, sq_ani = 0.0; int n = 0;
        for (int k = 0; k < 300; ++k) {
            double u = 0.1 + 0.8 * frand(), v = 0.1 + 0.8 * frand();
            double dudx = 8.0 / TILE, dvdy = 1.0 / TILE;
            TexReq r = {0};
            r.chain = &chain; r.atlas_w = AW; r.atlas_h = AH;
            r.tile_x = pos[0].x; r.tile_y = pos[0].y; r.tile_w = TILE; r.tile_h = TILE;
            r.bias_enabled = 1; r.bias = 0.0;
            r.u = u; r.v = v; r.dudx = dudx; r.dvdy = dvdy;
            r.mode = TEX_TRILINEAR;
            CK(tex_sample(&r, a) == 0);
            unsigned char tri[3]; tri[0] = a[0]; tri[1] = a[1]; tri[2] = a[2];
            r.mode = TEX_ANISO;
            CK(tex_sample(&r, a) == 0);
            CK(tex_ref_sample(&chain, AW, AH, pos[0].x, pos[0].y, TILE, TILE,
                              u, v, dudx, 0.0, 0.0, dvdy, b) == 0);
            for (int ch = 0; ch < 3; ++ch) {
                int dt = (int)tri[ch] - (int)b[ch];
                int da = (int)a[ch] - (int)b[ch];
                sq_tri += (double)(dt * dt);
                sq_ani += (double)(da * da);
                ++n;
            }
        }
        printf("anisotropic RMSE: aniso=%.3f trilinear=%.3f over %d channels\n",
               sqrt(sq_ani / n), sqrt(sq_tri / n), n);
        CK(sq_ani <= sq_tri * 1.05);
    }

    mip_free(&chain);
    mip_free(&idchain);
    free(atlas);
    free(tiles);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
