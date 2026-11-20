#ifndef CULL_H
#define CULL_H

/* 静态包围盒：原接口保持不变（不扩展时集合照旧）。 */
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ------------------------------------------------------------------ */
/* 运动感知剔除：两阶段                                                */
/*   1) cull_expand_boxes  按运动方向扩包围盒                          */
/*   2) cull_run_swept     按扩展后盒剔除                              */
/* 也可以用 cull_motion 一次完成（内部仍按两阶段）。                    */
/*                                                                    */
/* speed  : 速率，单位 像素/帧，必须 >= 0（负值报错，见 CULL_E_NEG）  */
/* dx, dy : 运动方向向量，无需单位化（内部归一化）                     */
/* ratio  : 扩展比例，可配。ratio<=0 表示不扩展，走原 cull_run 逻辑， */
/*          结果集合与旧接口逐对象一致，且不校验速度/方向。            */
/* 扩展像素数（每对象、每轴）写入 CullMotionReport，向量化报告。      */
/* 序号一律采用从 0 开始的数组下标；报错时同时给出“第几个”(idx+1)。   */
/* ------------------------------------------------------------------ */
typedef struct {
    double x, y, w, h;
    int    layer;
    double speed;   /* 像素/帧，>= 0 */
    double dx, dy;  /* 方向分量，可非单位 */
} CullMovingBox;

typedef struct {
    double *ex;      /* 每对象 x 轴单侧扩展像素数，长度 n */
    double *ey;      /* 每对象 y 轴单侧扩展像素数，长度 n */
    int     n;
} CullMotionReport;

/* 返回码 */
#define CULL_OK        0
#define CULL_E_ARG    -1   /* 空指针 / n<0 / 内存失败 */
#define CULL_E_VIEW   -2   /* 视口宽高非法 */
#define CULL_E_NEG    -3   /* speed<0；err_index 为对象下标(0起) */
#define CULL_E_ZERO   -4   /* 需扩展但扩展为 0：speed==0 或零方向；err_index 为下标(0起) */

/* 阶段 1：按运动方向扩包围盒，扩展盒写入 expanded，扩展像素写入 report。
 * 仅做扩盒，不做剔除。expanded 至少容纳 n 个 CullBox。*/
int cull_expand_boxes(const CullMovingBox *boxes, int n, double ratio,
                      CullBox *expanded, CullMotionReport *report, int *err_index);

/* 阶段 2：对（已扩展的）盒做视口剔除。 */
int cull_run_swept(const CullBox *expanded, int n, CullView view, CullResult *out);

/* 两阶段合一。ratio<=0 时退化为原静态剔除（按原始盒），不校验速度。 */
int cull_motion(const CullMovingBox *boxes, int n, CullView view,
                double ratio, CullResult *out,
                CullMotionReport *report, int *err_index);

/* 释放报告内部分配的数组。 */
void cull_report_free(CullMotionReport *report);

#endif
