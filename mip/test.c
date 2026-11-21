#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static unsigned char *make_image(int w, int h) {
    unsigned char *src = (unsigned char*)malloc((size_t)w*h*3);
    for (int i = 0; i < w*h; ++i) {
        src[i*3+0] = (unsigned char)(i % 256);
        src[i*3+1] = 40;
        src[i*3+2] = 200;
    }
    return src;
}

int main(void) {
    enum { W = 16, H = 16 };
    unsigned char *src = make_image(W, H);

    /* ---- 旧接口回归 ---- */
    MipChain c;
    CK(mip_build(&c, src, W, H) == 0);
    CK(c.levels == 5);
    CK(c.w[1] == 8 && c.h[1] == 8);
    unsigned char px[3];
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(mip_sample(&c, 9, 0.5, 0.5, px) == -1);

    /* ---- 缓存初始化与参数校验 ---- */
    MipError e;
    MipLayerCache cache;
    CK(mip_cache_init(&cache, &c, &e) == MIP_OK);
    CK(mip_cache_enabled(&cache) == 1);
    CK(mip_cache_init(NULL, &c, &e) == MIP_ERR_ARG);
    CK(e.field && strcmp(e.field, "cache") == 0 && e.index == -1);
    MipChain empty;
    memset(&empty, 0, sizeof empty);
    CK(mip_cache_init(&cache, &empty, &e) == MIP_ERR_CACHE);
    CK(e.field && strcmp(e.field, "chain") == 0);

    /* ---- 默认配置：批量结果与旧接口逐字节一致，覆盖所有层与多个位置 ---- */
    const double us[] = {0.0, 0.13, 0.5, 0.87, 1.0, -0.25, 1.75};
    const double vs[] = {0.0, 0.31, 0.5, 0.94, 1.0, 0.62, -0.1};
    int mismatch = 0;
    for (int l = 0; l < c.levels; ++l) {
        for (size_t k = 0; k < sizeof(us)/sizeof(us[0]); ++k) {
            unsigned char a[3], b[3];
            CK(mip_sample(&c, l, us[k], vs[k], a) == 0);
            int levels[1] = {l};
            CK(mip_batch_sample(&cache, levels, 1, us[k], vs[k], b, NULL, &e) == MIP_OK);
            if (memcmp(a, b, 3) != 0) ++mismatch;
        }
    }
    CK(mismatch == 0);

    /* 关闭缓存也必须字节一致 */
    mip_cache_set_enabled(&cache, 0);
    for (int l = 0; l < c.levels; ++l) {
        unsigned char a[3], b[3];
        CK(mip_sample(&c, l, 0.3, 0.7, a) == 0);
        int levels[1] = {l};
        CK(mip_batch_sample(&cache, levels, 1, 0.3, 0.7, b, NULL, &e) == MIP_OK);
        if (memcmp(a, b, 3) != 0) ++mismatch;
    }
    CK(mismatch == 0);
    mip_cache_set_enabled(&cache, 1);

    /* ---- 两段接口：选层结果与链一致 ---- */
    MipLayer layer;
    CK(mip_layer_select(&cache, 2, &layer, &e) == MIP_OK);
    CK(layer.level == 2 && layer.w == c.w[2] && layer.h == c.h[2]);
    CK(layer.bytes == (size_t)c.w[2]*c.h[2]*3 && layer.pix == c.pix[2]);

    /* ---- 批量选层：错误索引指向数组位置 ---- */
    int bad[] = {0, 1, 99, 2};
    MipLayer layers[4];
    CK(mip_layer_select_list(&cache, bad, 4, layers, &e) == MIP_ERR_LEVEL);
    CK(e.field && strcmp(e.field, "levels") == 0);
    CK(e.index == 2 && e.level == 99);
    int neg[] = {0, -1};
    CK(mip_layer_select_list(&cache, neg, 2, layers, &e) == MIP_ERR_LEVEL);
    CK(e.index == 1 && e.level == -1);
    /* 批内出错时前面已合法项也不写入 */
    MipLayer sentinel[4];
    memset(sentinel, 0x5A, sizeof sentinel);
    CK(mip_layer_select_list(&cache, bad, 4, sentinel, &e) == MIP_ERR_LEVEL);
    CK(sentinel[0].w == 0x5A5A5A5A);
    /* 合法选择仍然正常写入 */
    int good[2] = {0, 1};
    CK(mip_layer_select_list(&cache, good, 2, layers, &e) == MIP_OK);
    CK(layers[0].level == 0 && layers[1].level == 1);
    /* 单层选择：索引按层号报告 */
    CK(mip_layer_select(&cache, 7, &layer, &e) == MIP_ERR_LEVEL);
    CK(e.index == 7 && e.level == 7);

    /* ---- 批量采样越界：索引指向批量数组位置，不写半截输出 ---- */
    unsigned char out[15];
    memset(out, 0xAB, sizeof out);
    int ok_then_bad[] = {0, 12};
    CK(mip_batch_sample(&cache, ok_then_bad, 2, 0.5, 0.5, out, NULL, &e) == MIP_ERR_LEVEL);
    CK(e.index == 1 && e.level == 12);
    CK(out[0] == 0xAB && out[1] == 0xAB);
    int bad_first[] = {-3, 0};
    CK(mip_batch_sample(&cache, bad_first, 2, 0.5, 0.5, out, NULL, &e) == MIP_ERR_LEVEL);
    CK(e.index == 0 && e.level == -3);

    /* ---- 输入不合法：空指针 / 计数 / 非有限 UV / 无效层结构 ---- */
    CK(mip_batch_sample(&cache, NULL, 1, 0.5, 0.5, out, NULL, &e) == MIP_ERR_ARG);
    CK(e.field && strcmp(e.field, "levels") == 0);
    CK(mip_batch_sample(&cache, bad, -1, 0.5, 0.5, out, NULL, &e) == MIP_ERR_ARG);
    CK(e.field && strcmp(e.field, "count") == 0);
    CK(mip_batch_sample(&cache, bad, 1, 0.5, 0.5, NULL, NULL, &e) == MIP_ERR_ARG);
    CK(e.field && strcmp(e.field, "out") == 0);
    int one[] = {0};
    CK(mip_batch_sample(&cache, one, 1, NAN, 0.5, out, NULL, &e) == MIP_ERR_UV);
    CK(e.field && strcmp(e.field, "u") == 0);
    CK(mip_batch_sample(&cache, one, 1, 0.5, INFINITY, out, NULL, &e) == MIP_ERR_UV);
    CK(e.field && strcmp(e.field, "v") == 0);
    CK(mip_layer_sample(NULL, 0.5, 0.5, out, &e) == MIP_ERR_ARG);
    CK(e.field && strcmp(e.field, "layer") == 0);
    /* 极端但有限的 u/v 不许越界转换；按钳制语义落到边缘纹素 */
    CK(mip_layer_sample(&layers[0], 1e300, -1e300, out, &e) == MIP_OK);
    CK(memcmp(out, c.pix[0] + (size_t)(c.w[0]-1)*3, 3) == 0); /* v 为负 -> 第 0 行 */
    CK(mip_batch_sample(&cache, one, 1, 1e300, 1e300, out, NULL, &e) == MIP_OK);
    CK(memcmp(out, c.pix[0] + (size_t)(c.w[0]-1 + (c.h[0]-1)*c.w[0])*3, 3) == 0);
    MipLayer broken = {3, 4, 4, 48, NULL};
    CK(mip_layer_sample_same(&broken, 1, 0.5, 0.5, out, &e) == MIP_ERR_ARG);
    CK(e.index == 0 && e.level == 3);

    /* ---- 统计：层数 / 采样次数 / 命中 ----
     * 换一个新缓存，保证冷启动的命中/未命中计数可预期。 */
    mip_cache_free(&cache);
    CK(mip_cache_init(&cache, &c, &e) == MIP_OK);
    mip_cache_reset_stats(&cache);
    int all[] = {0, 1, 2, 3, 4};
    MipStats st;
    CK(mip_batch_sample(&cache, all, 5, 0.2, 0.4, out, &st, &e) == MIP_OK);
    long before_hits = st.hits, before_misses = st.misses;
    CK(mip_batch_sample(&cache, all, 5, 0.6, 0.8, out, &st, &e) == MIP_OK);
    CK(st.levels == 5);
    CK(st.batches == 2);
    CK(st.samples == 10);
    CK(st.hits - before_hits == 5);
    CK(st.misses == 5);
    CK(before_misses == 5 && before_hits == 0);
    /* 重复层号：同一批里第二次起算命中 */
    int dup[] = {1, 1, 1};
    CK(mip_batch_sample(&cache, dup, 3, 0.5, 0.5, out, &st, &e) == MIP_OK);
    CK(st.samples == 13);
    CK(st.misses == 5 && st.hits == 8);
    /* 关闭缓存：全部计为 miss，结果不变 */
    mip_cache_set_enabled(&cache, 0);
    CK(mip_batch_sample(&cache, dup, 3, 0.5, 0.5, out, &st, &e) == MIP_OK);
    CK(st.hits == 8 && st.misses == 8);
    mip_cache_set_enabled(&cache, 1);

    /* ---- 层表：内容与校验和，导出 CSV ---- */
    MipLayerTable t;
    CK(mip_layer_table_build(&c, &t, &e) == MIP_OK);
    CK(t.count == c.levels);
    int table_ok = 1;
    for (int l = 0; l < t.count; ++l) {
        if (t.layers[l].level != l || t.layers[l].w != c.w[l] || t.layers[l].h != c.h[l]) table_ok = 0;
        if (t.layers[l].bytes != (size_t)c.w[l]*c.h[l]*3) table_ok = 0;
        /* 独立重算校验和 */
        unsigned int h = 2166136261u;
        for (size_t i = 0; i < t.layers[l].bytes; ++i) { h ^= t.layers[l].pix[i]; h *= 16777619u; }
        if (t.checksum[l] != h) table_ok = 0;
    }
    CK(table_ok);
    CK(mip_layer_table_write_csv(&t, "/tmp/mip_layers.csv", &e) == MIP_OK);
    FILE *f = fopen("/tmp/mip_layers.csv", "rb");
    CK(f != NULL);
    if (f) {
        char line[256];
        int rows = 0;
        int header_ok = 0;
        while (fgets(line, sizeof line, f)) {
            if (rows == 0) header_ok = strstr(line, "level,width,height,bytes,checksum_fnv1a") != NULL;
            ++rows;
        }
        fclose(f);
        CK(header_ok);
        CK(rows == 1 + c.levels);
    }
    CK(mip_layer_table_write_csv(&t, "/nonexistent-dir/x.csv", &e) == MIP_ERR_IO);
    CK(e.field && strcmp(e.field, "path") == 0);
    CK(mip_layer_table_build(NULL, &t, &e) == MIP_ERR_ARG);
    mip_layer_table_free(&t);
    mip_cache_free(&cache);
    mip_free(&c);

    /* ---- 报错后缓存可继续使用，已选层不受影响 ---- */
    unsigned char *src2 = make_image(8, 8);
    CK(mip_build(&c, src2, 8, 8) == 0);
    CK(mip_cache_init(&cache, &c, &e) == MIP_OK);
    int levels_bad[] = {0, 40};
    CK(mip_batch_sample(&cache, levels_bad, 2, 0.5, 0.5, out, NULL, &e) == MIP_ERR_LEVEL);
    CK(mip_batch_sample(&cache, one, 1, 0.5, 0.5, out, &st, &e) == MIP_OK);
    CK(mip_sample(&c, 0, 0.5, 0.5, px) == 0);
    CK(out[0] == px[0] && out[1] == px[1] && out[2] == px[2]);
    mip_cache_free(&cache);
    mip_free(&c);
    free(src2);

    free(src);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
