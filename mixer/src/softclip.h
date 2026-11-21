#ifndef SOFTCLIP_H
#define SOFTCLIP_H

/*
 * 两段式软削波器。
 * 第一段 sc_build_curve: 按拐点 (knee) 预计算单调软削波曲线查找表;
 * 第二段 sc_apply:        对交错声道缓冲逐样本套用 (线性插值)。
 *
 * 曲线 (奇对称, 对 x >= 0):
 *   f(x) = x                                        , 0 <= x <= T
 *   f(x) = C - (C - T) * exp(-(x - T) / (C - T))    , x > T
 * 其中 T = knee (拐点), C = ceiling (输出上限)。拐点处 f(T)=T、f'(T)=1,
 * 与线性段 C1 连续 (指数尺度取 C-T 才能同时满足这三个约束);
 * x -> inf 时 f(x) -> C, 不会硬切。
 * T == C 时退化为在 T 处的硬削波 (仍然合法)。
 */

typedef struct {
    float knee;     /* 拐点 T: 超过该幅度开始压缩, 必须 > 0 */
    float ceiling;  /* 输出上限 C: 必须 >= T */
    int   tabs;     /* 查找表点数 (含 0 点), 必须 >= 4 */
    float *tab;     /* tabs 个点, 覆盖 [0, knee] 的输出值 */
    int   valid;    /* 1 = 已成功构建, 0 = 未启用/无效 (此时 sc_apply 直通) */
} SoftClip;

/* 冲突位掩码: sc_build_curve 返回 -2 时, *conflicts 取以下位的或 */
enum {
    SC_CONF_KNEE_NONPOS = 1 << 0,  /* knee <= 0 */
    SC_CONF_CEIL_LT_KNEE = 1 << 1, /* ceiling < knee */
    SC_CONF_CEIL_NONPOS = 1 << 2,  /* ceiling <= 0 */
    SC_CONF_TABS_SMALL = 1 << 3,   /* tabs < 4 */
    SC_CONF_KNEE_NANINF = 1 << 4,  /* knee 为 NaN/Inf */
    SC_CONF_CEIL_NANINF = 1 << 5,  /* ceiling 为 NaN/Inf */
    SC_CONF_NULL = 1 << 6          /* sc 为空指针 */
};

/* 解析解: 给定曲线参数, 返回输入 x 的解析输出 (不依赖查找表)。
 * 参数非法时返回 0。供曲线表导出与逐项对照使用。 */
float sc_analytic(float knee, float ceiling, float x);

/* 第一段: 校验参数并按拐点构建查找表。
 * 返回 0 成功; -1 空指针; -2 非法组合 (*conflicts 列出全部冲突项)。
 * 失败时 sc->valid 置 0, 不分配内存。 */
int sc_build_curve(SoftClip *sc, float knee, float ceiling, int tabs,
                   unsigned *conflicts);

/* 释放查找表并复位为未启用状态。 */
void sc_free(SoftClip *sc);

/* 第二段: 对 frames*channels 个交错样本逐样本套用曲线。
 * sc 为空或 valid==0 时直通 (输入原样写回), 与未开启软削波完全一致。
 * 返回处理的样本总数 (frames*channels), 参数非法返回 -1。 */
long sc_apply(const SoftClip *sc, float *buf, int frames, int channels);

/* 导出曲线表 CSV: 每行 "input,analytic,lut", 采样 n 点覆盖 [-xmax, xmax]。
 * 返回 0 成功, 负值失败。 */
int sc_export_csv(const SoftClip *sc, const char *path, int n, float xmax);

#endif
