#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return -1;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static int point_shadowed(const ShSeg *segs, int nseg,
                          float lx, float ly, float px, float py) {
    for (int i = 0; i < nseg; ++i)
        if (blocked(&segs[i], lx, ly, px, py)) return 1;
    return 0;
}

static float point_light(const ShLight *light, const ShSeg *segs, int nseg,
                         float px, float py) {
    if (point_shadowed(segs, nseg, light->x, light->y, px, py)) return 0.0f;
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    float fall = 1.0f / (1.0f + d / light->radius);
    return light->intensity * fall;
}

/* ---- 抖动矩阵 ----------------------------------------------------------- */

static int valid_tile(int tile) { return tile == 2 || tile == 4 || tile == 8; }
static int valid_type(int t) {
    return t == SH_JITTER_NONE || t == SH_JITTER_BAYER ||
           t == SH_JITTER_ROTATED || t == SH_JITTER_CUSTOM;
}

/* 生成 tile x tile 的标准有序 Bayer 矩阵，值归一化到 [0,1)。 */
static void bayer_matrix(int tile, float *out) {
    static const int b2[4] = {0, 2, 3, 1};
    int n = 1;
    int *cur = (int*)malloc(sizeof(int) * (size_t)tile * (size_t)tile);
    int *nxt = NULL;
    cur[0] = 0;
    while (n < tile) {
        int nn = n * 2;
        nxt = (int*)malloc(sizeof(int) * (size_t)nn * (size_t)nn);
        for (int y = 0; y < nn; ++y)
            for (int x = 0; x < nn; ++x) {
                int base = 4 * cur[(y >> 1) * n + (x >> 1)];
                nxt[y * nn + x] = base + b2[(y & 1) * 2 + (x & 1)];
            }
        free(cur);
        cur = nxt;
        n = nn;
    }
    float scale = 1.0f / (float)(tile * tile);
    for (int i = 0; i < tile * tile; ++i) out[i] = (cur[i] + 0.5f) * scale;
    free(cur);
}

/* 位反转：把 n 位整数的二进制位倒序。用于在 tile^2 个槽位间做双射置换。 */
static unsigned bit_reverse(unsigned x, int bits) {
    unsigned r = 0;
    for (int i = 0; i < bits; ++i) { r = (r << 1) | (x & 1u); x >>= 1; }
    return r;
}

/* 打散版 Bayer：对标准 Bayer 的线性索引做位反转置换。
 * 位反转是双射（每个目标槽位恰好被写一次，无重复、无遗漏），同时把矩阵
 * 行列方向上的规则低频结构换成高频分散排列，从而弱化规则网格纹。 */
static void rotated_matrix(int tile, float *out) {
    float *base = (float*)malloc(sizeof(float) * (size_t)tile * (size_t)tile);
    bayer_matrix(tile, base);
    int bits = 0;
    for (int t = tile; t > 1; t >>= 1) ++bits;   /* tile 边长的位数 */
    int total_bits = bits * 2;                     /* tile^2 个索引的位数 */
    for (int i = 0; i < tile * tile; ++i)
        out[bit_reverse((unsigned)i, total_bits)] = base[i];
    free(base);
}

int sh_jitter_build(int matrix_type, int tile, float *out) {
    if (!out || !valid_tile(tile)) return -1;
    if (matrix_type == SH_JITTER_BAYER) { bayer_matrix(tile, out); return 0; }
    if (matrix_type == SH_JITTER_ROTATED) { rotated_matrix(tile, out); return 0; }
    return -1; /* NONE/CUSTOM 没有可生成的内置矩阵 */
}

static const char *type_name(int t) {
    switch (t) {
        case SH_JITTER_NONE: return "SH_JITTER_NONE";
        case SH_JITTER_BAYER: return "SH_JITTER_BAYER";
        case SH_JITTER_ROTATED: return "SH_JITTER_ROTATED";
        case SH_JITTER_CUSTOM: return "SH_JITTER_CUSTOM";
        default: return "SH_JITTER_UNKNOWN";
    }
}

int sh_jitter_validate(const ShJitterCfg *cfg, char *err, int errcap) {
    if (!cfg) return -1;
    #define SH_CONFLICT(...) do { \
        if (err && errcap > 0) snprintf(err, (size_t)errcap, "config conflict: " __VA_ARGS__); \
        return -3; } while (0)

    if (!cfg->enabled) {
        if (cfg->matrix_type != SH_JITTER_NONE)
            SH_CONFLICT("enabled=0 vs matrix_type=%s (expected SH_JITTER_NONE)",
                        type_name(cfg->matrix_type));
        return 0;
    }
    if (cfg->matrix_type == SH_JITTER_NONE)
        SH_CONFLICT("enabled=1 vs matrix_type=SH_JITTER_NONE (two sides disagree on jitter)");
    if (!valid_type(cfg->matrix_type))
        SH_CONFLICT("matrix_type=%d vs allowed {BAYER,ROTATED,CUSTOM}", cfg->matrix_type);
    if (cfg->tier != SH_TIER_FAST && cfg->tier != SH_TIER_HIGH)
        SH_CONFLICT("tier=%d vs allowed {SH_TIER_FAST,SH_TIER_HIGH}", cfg->tier);
    if (!valid_tile(cfg->tile))
        SH_CONFLICT("tile=%d vs allowed {2,4,8}", cfg->tile);
    if (cfg->matrix_type == SH_JITTER_CUSTOM && !cfg->mat)
        SH_CONFLICT("matrix_type=SH_JITTER_CUSTOM vs mat=NULL (custom type demands a matrix)");
    if (cfg->matrix_type != SH_JITTER_CUSTOM && cfg->mat != NULL)
        SH_CONFLICT("built-in matrix_type=%s vs non-NULL mat (only CUSTOM may carry a matrix)",
                    type_name(cfg->matrix_type));
    /* 档位与边长配套：低速 2x2（4 点），高速 8x8（64 点）。 */
    if (cfg->tier == SH_TIER_FAST && cfg->tile != 2)
        SH_CONFLICT("tier=SH_TIER_FAST vs tile=%d (FAST requires tile=2)", cfg->tile);
    if (cfg->tier == SH_TIER_HIGH && cfg->tile != 8)
        SH_CONFLICT("tier=SH_TIER_HIGH vs tile=%d (HIGH requires tile=8)", cfg->tile);
    return 0;
    #undef SH_CONFLICT
}

/* 文本格式：首行 magic/type/tile/count，其后每行一个 [0,1) 偏移，行主序。 */
int sh_jitter_export(const char *path, int matrix_type, int tile, const float *mat) {
    if (!path || !mat || !valid_tile(tile)) return -1;
    for (int i = 0; i < tile * tile; ++i)
        if (!(mat[i] >= 0.0f && mat[i] < 1.0f)) return -3; /* 顺带挡住 NaN 与负零 */
    FILE *f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f, "SHJIT1 type=%d tile=%d count=%d\n",
            matrix_type, tile, tile * tile);
    for (int i = 0; i < tile * tile; ++i) {
        float v = mat[i] + 0.0f; /* 归一化负零，避免写出 "-0.000000" */
        fprintf(f, "%.6f\n", v);
    }
    fclose(f);
    return 0;
}

/* ---- 渲染 --------------------------------------------------------------- */

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            m->buf[(size_t)y * m->w + x] = point_light(light, segs, nseg, px, py);
        }
    return 0;
}

/* 第一段：把第 k 个规则网格采样点按抖动矩阵扰动到子像素位置。
 * 像素被均分成 side*side 个子格，采样点原在子格中心；矩阵偏移把它
 * 限制在本子格内（幅度 ±1/(2*side)），保持分层采样不重叠的性质。 */
static void jittered_spot(int k, int side, const float *mat, int tile,
                          float *ox, float *oy) {
    int gx = k % side, gy = k / side;
    float base_x = ((float)gx + 0.5f) / (float)side;
    float base_y = ((float)gy + 0.5f) / (float)side;
    int cx = gx % tile, cy = gy % tile;
    float jx = (mat[cy * tile + cx] - 0.5f) / (float)side;
    /* y 方向换一条索引，避免两个方向共用同一抖动序列。 */
    float jy = (mat[((cx + cy) % tile) * tile + cx] - 0.5f) / (float)side;
    *ox = base_x + jx;
    *oy = base_y + jy;
}

/* 第二段：在给定像素上对扰动后的采样点逐个求光照并平均。 */
static float average_jittered(const ShLight *light, const ShSeg *segs, int nseg,
                              int px0, int py0, int side, int tile,
                              const float *mat) {
    int nsamp = side * side;
    float acc = 0.0f;
    for (int k = 0; k < nsamp; ++k) {
        float ox, oy;
        jittered_spot(k, side, mat, tile, &ox, &oy);
        acc += point_light(light, segs, nseg, (float)px0 + ox, (float)py0 + oy);
    }
    return acc / (float)nsamp;
}

int sh_render_jit(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                  int nseg, const ShJitterCfg *cfg, int *out_nsamp) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;

    /* 开关为假：走与 sh_render 完全一致的单点路径，行为不变。 */
    if (!cfg || !cfg->enabled) {
        if (out_nsamp) *out_nsamp = 1;
        return sh_render(m, light, segs, nseg);
    }

    char err[160];
    int vc = sh_jitter_validate(cfg, err, (int)sizeof(err));
    if (vc != 0) return vc;

    int tile = cfg->tile;
    int side = tile;
    int nsamp = side * side;
    if (out_nsamp) *out_nsamp = nsamp;

    float *mat;
    int owned = 0;
    if (cfg->matrix_type == SH_JITTER_CUSTOM) {
        mat = (float*)cfg->mat;
    } else {
        mat = (float*)malloc(sizeof(float) * (size_t)nsamp);
        if (!mat) return -9;
        if (sh_jitter_build(cfg->matrix_type, tile, mat) != 0) { free(mat); return -9; }
        owned = 1;
    }

    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x)
            m->buf[(size_t)y * m->w + x] =
                average_jittered(light, segs, nseg, x, y, side, tile, mat);

    if (owned) free(mat);
    return 0;
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}
