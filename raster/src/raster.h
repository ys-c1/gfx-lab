#ifndef RASTER_H
#define RASTER_H
#include <stddef.h>
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;

/* 渲染参数开关：rt_set_config 校验，越界返回 -3 并通过 err_field 指明字段 */
typedef struct {
    int enable_prepass;   /* 0=单遍(默认,与旧行为逐像素一致); 1=深度预通道+颜色遍 */
    int track_stats;      /* 0/1: 是否统计每遍墙钟耗时 */
    float depth_eps;      /* 深度拒绝容差, 有限值且 [0,1]; 0 时与旧深度测试位精确一致 */
} RConfig;

typedef struct {
    long covered;         /* 三角形覆盖到的像素评估数(每像素计一次) */
    long depth_hits;      /* 深度缓冲更新次数 */
    long rejected;        /* 颜色遍被深度拒绝的像素数(仅预通道模式) */
    long written;         /* 颜色写入次数 */
    double prepass_sec;   /* 只写深度预通道耗时(track_stats 时有效) */
    double color_sec;     /* 颜色遍(单遍模式即整遍)耗时 */
} RStats;

typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    RConfig cfg;
    RStats stats;
    float *zprev;         /* 预通道前的旧深度快照, 仅 enable_prepass 时分配 */
} RTarget;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_clear(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_write_ppm(const RTarget *t, const char *path);

void rt_config_default(RConfig *cfg);
/* 0=合法; -1=空指针; -3=参数越界, err_field 写出出错字段名(缓冲足够时) */
int rt_config_validate(const RConfig *cfg, char *err_field, size_t err_field_cap);
/* -1=空指针; -3=参数越界; -4=快照缓冲分配失败 */
int rt_set_config(RTarget *t, const RConfig *cfg, char *err_field, size_t err_field_cap);
void rt_reset_stats(RTarget *t);
int rt_get_stats(const RTarget *t, RStats *out);
#endif
