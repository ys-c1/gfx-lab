#ifndef NINEPATCH_H
#define NINEPATCH_H

/* 九宫格策略：边块拉伸或平铺 */
#define NP_STRETCH 0
#define NP_TILE    1

typedef struct {
    int enabled;          /* 0: 关闭九宫格，整图按最近邻缩放到目标尺寸 */
    int left, right;      /* 源图块左/右边宽（像素） */
    int top, bottom;      /* 源图块上/下边宽（像素） */
    int h_strategy;       /* 上/下水平边策略：NP_STRETCH 或 NP_TILE */
    int v_strategy;       /* 左/右垂直边策略：NP_STRETCH 或 NP_TILE */
} NpConfig;

/*
 * 将 w*h 的 RGB 图块（每像素 3 字节，紧凑行优先）渲染到 dw*dh 的目标缓冲。
 * 开启九宫格时：四角直接拷贝，四边按策略拉伸/平铺，中心双向拉伸。
 * 返回 0 成功；负值表示参数错误：
 *   -1 空指针或尺寸非法
 *   -2 边宽超过图块或切分越界（left/right/top/bottom 字段）
 *   -3 策略非法（h_strategy/v_strategy 字段）
 */
int np_render(const unsigned char *src, int w, int h,
              const NpConfig *cfg, unsigned char *dst, int dw, int dh);

int np_write_ppm(const unsigned char *rgb, int w, int h, const char *path);
#endif
