#ifndef TEX_H
#define TEX_H
#include "mip.h"

/* 采样模式：bias_enabled == 0 时恒为 LEGACY 路径，mode 被忽略 */
typedef enum {
    TEX_LEGACY_POINT = 0, /* 固定层号点采样，行为与 mip_sample 一致 */
    TEX_TRILINEAR    = 1, /* 导数定层 + 相邻两层双线性混合 */
    TEX_ANISO        = 2  /* 按长宽比拆子样本，每个子样本走三线性 */
} TexMode;

#define TEX_MAX_ANISO 8

typedef struct {
    const MipChain *chain;        /* 字段名 "chain"：图集位图构建出的 mip 链 */
    int atlas_w, atlas_h;         /* 字段名 "atlas_w"/"atlas_h"：基图层图集尺寸 */
    int tile_x, tile_y;           /* 字段名 "tile_x"/"tile_y"：图块在图集中的基图层原点 */
    int tile_w, tile_h;           /* 字段名 "tile_w"/"tile_h"：图块基图层宽高 */
    TexMode mode;                 /* 字段名 "mode" */
    int bias_enabled;             /* 字段名 "bias_enabled"：0 => 旧路径，输出与现状相同 */
    double bias;                  /* 字段名 "bias"：LOD 偏置，启用时必须有限 */
    int level;                    /* 字段名 "level"：legacy 固定层号 */
    double u, v;                  /* 字段名 "u"/"v"：图块局部 UV，[0,1) */
    double dudx, dudy;            /* 字段名 "dudx"/"dudy"：u 对屏幕坐标的导数 */
    double dvdx, dvdy;            /* 字段名 "dvdx"/"dvdy"：v 对屏幕坐标的导数 */
    long taps;                    /* 输出：本次采样消耗的基图层点采样次数 */
} TexReq;

/*
 * 返回值：
 *   0  成功
 *  -1  空指针（字段 "chain"/"out"）
 *  -2  图块越界（字段名指向 tile_x/tile_y/tile_w/tile_h/atlas_w/atlas_h）
 *  -3  数值非有限（字段名指向 bias/u/v/dudx/dudy/dvdx/dvdy）
 *  -4  层号越界（legacy 字段 "level"，导数模式字段 "lod"）
 *  -5  非法 mode（字段 "mode"）
 * 出错时同时向 stderr 打印一行，包含具体字段名。
 */
int tex_sample(TexReq *r, unsigned char out[3]);

#endif
