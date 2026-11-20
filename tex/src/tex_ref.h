#ifndef TEX_REF_H
#define TEX_REF_H
#include "tex.h"

/*
 * 自建参考采样器（ground truth，仅供对账，不进入生产链）：
 * 只用基图层像素，按屏幕导数反推图块内 texel 覆盖盒，做面积加权平均；
 * 放大时回落到双线性。与 tex.c 的 LOD/层混合逻辑完全独立，便于交叉对账。
 * 字段语义与 TexReq 中同名字段一致。返回值约定与 tex_sample 相同。
 */
int tex_ref_sample(const MipChain *chain, int atlas_w, int atlas_h,
                   int tile_x, int tile_y, int tile_w, int tile_h,
                   double u, double v, double dudx, double dudy,
                   double dvdx, double dvdy, unsigned char out[3]);
#endif
