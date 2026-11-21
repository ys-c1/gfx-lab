#ifndef MIP_H
#define MIP_H

#include <stddef.h>

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);

/* ------------------------------------------------------------------
 * 两段式层采样
 *
 *   第一段：按层号选层，得到该层的宽度、高度、像素指针（带缓存）。
 *   第二段：对同一位置 (u, v) 批量采样一组已选好的层。
 *
 * 旧接口 mip_sample 的行为与返回码完全不变。
 * 默认配置下，mip_batch_sample 的每个输出都与 mip_sample 逐字节一致。
 * ------------------------------------------------------------------ */

enum {
    MIP_OK        = 0,
    MIP_ERR_ARG   = -1, /* 空指针或数量为负 */
    MIP_ERR_LEVEL = -2, /* 层号越界 */
    MIP_ERR_UV    = -3, /* u/v 不是有限数 */
    MIP_ERR_IO    = -4, /* 文件打开/写入失败 */
    MIP_ERR_CACHE = -5, /* 缓存与链不匹配或未初始化 */
    MIP_ERR_MEM   = -6  /* 内存分配失败 */
};

/*
 * 出问题时的定位：
 *   field  出问题的字段名（静态字符串），成功为 NULL；
 *   index  出问题的输入下标：批量层号数组中的位置；单层选择时为层号本身；
 *          与数组无关时为 -1；
 *   level  出问题的层号值，与层号无关时为 -1。
 * 批量采样在整组选层成功前不会写出任何字节，出错时输出缓冲保持原样。
 */
typedef struct {
    int code;
    const char *field;
    int index;
    int level;
} MipError;

/* 一层纹理的描述，pix 指向 MipChain 内部，生命周期跟随链。 */
typedef struct {
    int level;
    int w, h;
    size_t bytes;              /* w * h * 3 */
    const unsigned char *pix;
} MipLayer;

/* 层表：层号 -> 尺寸 / 字节数 / FNV-1a 校验和。可导出为 CSV。 */
typedef struct {
    int count;
    MipLayer *layers;
    unsigned int *checksum;
} MipLayerTable;

/* FNV-1a 32 位，层表导出与参考核对共用。 */
unsigned int mip_checksum(const unsigned char *data, size_t bytes);

int mip_layer_table_build(const MipChain *c, MipLayerTable *t, MipError *err);
void mip_layer_table_free(MipLayerTable *t);
/*
 * 写 CSV，表头为：
 *   level,width,height,bytes,checksum_fnv1a
 * 校验和写成 0x%08x。
 */
int mip_layer_table_write_csv(const MipLayerTable *t, const char *path, MipError *err);

/*
 * 选层缓存。cache_enabled 默认为 1；置 0 后每次选择都算 miss，
 * 但返回内容与结果字节不变（用于验证"默认/关闭缓存时与旧版一致"）。
 * 统计口径：
 *   selects  选层调用次数（含重复层号）
 *   hits     cache_enabled 且该层已选过
 *   misses   selects - hits
 *   samples  批量采样累计的层数（= 成功批次 × 每批层数）
 *   batches  成功的批量采样次数
 */
typedef struct {
    const MipChain *chain;
    int slot_count;            /* 初始化时的链层数 */
    MipLayer *slots;           /* slot_count 项 */
    unsigned char *bound;      /* slot_count 项，非零表示已选过 */
    MipLayer *frame;           /* mip_batch_sample 的临时选层数组 */
    int frame_cap;
    int cache_enabled;
    long batches;
    long selects;
    long hits;
    long misses;
    long samples;
} MipLayerCache;

typedef struct {
    int levels;
    long batches;
    long selects;
    long hits;
    long misses;
    long samples;
} MipStats;

int mip_cache_init(MipLayerCache *cache, const MipChain *chain, MipError *err);
void mip_cache_free(MipLayerCache *cache);
void mip_cache_set_enabled(MipLayerCache *cache, int enabled);
int mip_cache_enabled(const MipLayerCache *cache);
void mip_cache_reset_stats(MipLayerCache *cache);
void mip_cache_stats(const MipLayerCache *cache, MipStats *stats);

/* 第一段：选一层 / 选一组层。levels 中的重复项从第二次起算缓存命中。 */
int mip_layer_select(MipLayerCache *cache, int level, MipLayer *out, MipError *err);
int mip_layer_select_list(MipLayerCache *cache, const int *levels, int count,
                          MipLayer *out, MipError *err);

/* 第二段：单层采样 / 同一位置批量采样。输出每层 3 字节，紧密排列。 */
int mip_layer_sample(const MipLayer *layer, double u, double v,
                     unsigned char out[3], MipError *err);
int mip_layer_sample_same(const MipLayer *layers, int count, double u, double v,
                          unsigned char *out, MipError *err);

/* 两段合并的便捷入口：选层 -> 批量采样，并累计统计。 */
int mip_batch_sample(MipLayerCache *cache, const int *levels, int count,
                     double u, double v, unsigned char *out,
                     MipStats *stats, MipError *err);

#endif
