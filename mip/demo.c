#include "mip.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y*W+x)*3;
            p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4); p[2] = 128;
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;
    printf("levels %d sizes %dx%d %dx%d\n", c.levels, c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1]);

    /* 第一段：按层号选层 */
    MipLayerCache cache;
    MipError err;
    if (mip_cache_init(&cache, &c, &err) != MIP_OK) return 1;
    int levels[] = {0, 1, 2};
    MipLayer layers[3];
    if (mip_layer_select_list(&cache, levels, 3, layers, &err) != MIP_OK) return 1;
    for (int i = 0; i < 3; ++i)
        printf("layer %d: %dx%d %lu bytes\n",
               layers[i].level, layers[i].w, layers[i].h, (unsigned long)layers[i].bytes);

    /* 第二段：同一位置批量采样 */
    unsigned char out[3*3];
    if (mip_layer_sample_same(layers, 3, 0.5, 0.5, out, &err) != MIP_OK) return 1;
    for (int i = 0; i < 3; ++i)
        printf("sample layer %d: %d %d %d\n", levels[i], out[i*3], out[i*3+1], out[i*3+2]);

    /* 层表导出 */
    MipLayerTable table;
    if (mip_layer_table_build(&c, &table, &err) != MIP_OK) return 1;
    if (mip_layer_table_write_csv(&table, "demo_layers.csv", &err) != MIP_OK) return 1;
    printf("wrote demo_layers.csv (%d rows)\n", table.count);
    mip_layer_table_free(&table);

    /* 默认配置下与旧接口逐字节一致 */
    unsigned char a[3], b[3];
    mip_sample(&c, 1, 0.25, 0.75, a);
    mip_batch_sample(&cache, (int[]){1}, 1, 0.25, 0.75, b, NULL, &err);
    printf("legacy vs batch: %s\n", (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ? "identical" : "DIFF");

    mip_cache_free(&cache);
    mip_free(&c);
    free(src);
    return 0;
}
