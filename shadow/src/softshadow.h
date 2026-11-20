#ifndef SOFTSHADOW_H
#define SOFTSHADOW_H
#include "shadow.h"

/*
 * 软阴影（软边）三段式管线：
 *   A. sb_distance_field  : 按像素到最近遮挡物(线段)的距离，算每像素模糊半径
 *   B. sb_blur_separable  : 对阴影图做可分离高斯模糊（先水平后垂直两次一维卷积）
 *   C. sb_composite       : 合成最终图，并由 sb_measure_soft_edge 报软边宽度
 *
 * 不做模糊时（enable=0）管线输出与输入阴影图逐像素相同（见 sb_composite）。
 */

/* 边界处理（B 阶段采样越界时）：
 * CLAMP : 钳位到边缘像素，边缘外视为边缘像素的颜色（默认，能量不外泄）
 * ZERO  : 边界外补 0（阴影图里 0 即全黑/本影）
 */
typedef enum { SB_BORDER_CLAMP = 0, SB_BORDER_ZERO = 1 } SbBorderMode;

typedef struct {
    int   enable;          /* 0=关闭软边，画面照旧；非 0=启用 */
    int   blur_radius_px;  /* 字段：最大模糊半径 r（像素，>=0）。一维核长 = 2*r+1 */
    int   blur_radius_min; /* 字段：边界处的最小半径（基础软度，0<=min<=blur_radius_px） */
    float dist_begin;      /* 字段：模糊半径开始增长的距离（像素） */
    float dist_scale;      /* 字段：半径随距离增长的斜率（半径像素/距离像素） */
    float sigma_scale;     /* 字段：高斯 sigma = sigma_scale * r */
    int   border_mode;     /* 字段：SbBorderMode */
    long  sample_count;    /* 输出字段：B 阶段实际纹理采样（tap）次数，由管线累加 */
} SbConfig;

/* 错误码（均为负值）。出错时可用 sb_error_msg 取带字段名的说明 */
enum {
    SB_OK = 0,
    SB_ERR_ARG = -1,        /* 空指针等通用参数错误 */
    SB_ERR_SIGMA = -2,      /* sigma 非有限正数 */
    SB_ERR_RADIUS_ZERO = -3,/* enable 时字段 blur_radius_px 为 0 */
    SB_ERR_KERNEL_EVEN = -4,/* 核长为偶数（核长字段 kernel_len = 2*r+1，必须为奇数） */
    SB_ERR_BORDER = -5,     /* border_mode 字段非法 */
    SB_ERR_RADIUS_MIN = -6, /* blur_radius_min 越界（须 0<=min<=blur_radius_px） */
    SB_ERR_MEM = -7         /* 内存分配失败 */
};

const char *sb_error_msg(int code);

/* 统一配置校验：半径为零 -> SB_ERR_RADIUS_ZERO（字段 blur_radius_px）；
 * 核长为偶 -> SB_ERR_KERNEL_EVEN（字段 kernel_len = 2*blur_radius_px+1）。
 * 管线内部也会调用，外部可先行检查。 */
int sb_validate_config(const SbConfig *cfg);

/* 按字段直接校验（外部反序列化/美术填表时用）：
 * radius_px==0 -> SB_ERR_RADIUS_ZERO（字段 blur_radius_px）
 * kernel_len 偶 -> SB_ERR_KERNEL_EVEN（字段 kernel_len），且要求 kernel_len==2*radius_px+1 */
int sb_check_kernel_len(int radius_px, int kernel_len);

/* 导出一维模糊核：归一化高斯权重，长度 2*r+1（奇数）。
 * 返回核长；out 至少容纳 2*r+1 个 float。 */
int sb_kernel(int r, float sigma_scale, float *out);

/* 把核按文本导出到 path（每行一个权重，附参数与边界说明），便于美术/外部核对 */
int sb_kernel_export(const char *path, int r, float sigma_scale, int border_mode);

/* A 阶段：距离场 + 每像素模糊半径图。
 * dist / radius 均为 w*h 的调用方缓冲；*out_rmax 返回出现过的最大半径。 */
int sb_distance_field(const ShadowMap *m, const ShSeg *segs, int nseg,
                      const SbConfig *cfg, float *dist, float *radius, int *out_rmax);

/* B 阶段：可分离模糊。src->dst，临时缓冲内部申请。采样次数累加到 cfg->sample_count。
 * kernel 按全局最大半径 rmax 生成（长度 2*rmax+1）；某像素有效半径 r<rmax 时
 * 取该核的中心 2r+1 段并重新归一化，因此逐像素半径不同也只用同一条核。 */
int sb_blur_separable(const ShadowMap *src, ShadowMap *dst,
                      const float *radius, int rmax, const float *kernel,
                      SbConfig *cfg);

/* C 阶段：enable=0 时把 src 原样拷贝到 dst（画面照旧）；否则拷贝已模糊的 soft。 */
int sb_composite(const ShadowMap *hard, const ShadowMap *soft,
                 const SbConfig *cfg, ShadowMap *dst);

typedef struct {
    int   boundary_y;   /* 硬阴影边界所在行（亚像素，插值得到） */
    float width_px;     /* 软边宽度：10%~90% 过渡带的像素跨度 */
    float lo_y, hi_y;   /* 10% / 90% 交叉点的亚像素 y */
} SbSoftEdge;

/* 沿第 col 列（垂直穿过水平阴影边）测软边宽度 */
int sb_measure_soft_edge(const ShadowMap *hard, const ShadowMap *soft,
                         int col, SbSoftEdge *out);

/* 灰度 .pgm 输出（float 缓冲，钳到 [0,1]） */
int sb_write_pgm_f(const char *path, int w, int h, const float *buf);

#endif
