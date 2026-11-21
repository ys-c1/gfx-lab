#ifndef SHADOW_H
#define SHADOW_H

/* 方形/矩形面光软阴影。
 *
 * 三段管线：
 *   1. sh_sample_quad  按光源朝向在发光面上取多点（局部网格 -> 世界坐标）
 *   2. sh_composite    逐点做遮挡判定，按覆盖比例（未被遮挡的比例）合成亮度
 *   3. sh_penumbra     扫描覆盖图的本影/半影/全亮过渡带，报半影宽度
 *
 * 开关关闭（aw==ah==0 或 SH_GEAR_OFF）时完全走旧的点光源路径，
 * 与扩展接口引入前的 sh_render 输出逐位一致。
 */

typedef struct {
    float x, y;         /* 光源中心（世界坐标，y 轴向下） */
    float radius;       /* 光照衰减半径 */
    float intensity;    /* 光强 */
    float aw, ah;       /* 面光开关与尺寸：均为 0 = 点光源（旧行为）；
                         * 否则为矩形面光的局部半宽/半高，aw==ah 即正方形 */
    float angle;        /* 面光朝向（弧度，顺时针，screen y-down 旋转） */
} ShLight;

typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { float x, y; } ShPoint;
typedef struct { int w, h; float *buf; } ShadowMap;

/* 采样档位。OFF 保持点光源行为；FAST/SLOW 为高/低速两档；EXHAUST 为穷举参考。 */
enum ShGear {
    SH_GEAR_OFF = 0,
    SH_GEAR_FAST = 1,   /* 高速档：4x4 = 16 点 */
    SH_GEAR_SLOW = 2,   /* 低速档：12x12 = 144 点 */
    SH_GEAR_EXHAUST = 3 /* 穷举参考档：48x48 = 2304 点 */
};

typedef struct {
    int gear;           /* SH_GEAR_* */
    int cols, rows;     /* 面光上的采样网格（cols x rows） */
    float jitter;       /* 0 = 确定性网格；>0 = 以该幅度（局部边长比例）抖动 */
} ShRenderCfg;

/* 预设档位配置（可直接按 gear 取 sh_preset_cfg(gear)）。 */
const ShRenderCfg *sh_preset_cfg(int gear);

/* 半影测量结果：沿水平/垂直扫描线，本影(coverage=0)与全亮(coverage=1)之间
 * 过渡带的像素宽度。width_avg 只统计真正出现过渡带的扫描线。 */
typedef struct {
    float width_avg;    /* 水平+垂直过渡带平均宽度（像素），无过渡带时为 0 */
    float width_max;    /* 最大过渡带宽度（像素） */
    int   transitions;  /* 参与统计的过渡带条数 */
} ShPenumbra;

int  sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int  sh_clear(ShadowMap *m);

/* 旧接口：点光源硬阴影，行为保持不变。
 * 若对面光源（aw/ah>0）调用本接口，返回 -3 冲突错误（旧接口不支持面光）。 */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

int sh_write_pgm(const ShadowMap *m, const char *path);

/* 最近一次错误的可读信息（静态缓冲，非线程安全），无错误时返回空串。
 * 冲突类错误（返回 -3）的信息会指明冲突的两方，例如：
 *   "config conflict: light.aw/ah=... (area light ON) vs cfg.gear=OFF ..." */
const char *sh_last_error(void);

/* ---- 第 1 段：按光源朝向取多点 ----------------------------------------------
 * 在以光源中心为原点的发光面上生成 cols x rows 个采样点，并按 angle 旋转到
 * 世界坐标。调用方传入容量 >= cols*rows 的 out 数组；采样点分布也可通过
 * sh_local_point 单独取到（局部坐标，未旋转）。
 * 返回采样点数；参数非法返回 -1（并设置 sh_last_error）。 */
int sh_sample_quad(const ShLight *light, const ShRenderCfg *cfg, ShPoint *out);

/* 取网格中第 idx 个采样点在面光局部坐标系下的位置（局部坐标落在
 * [-1,1]^2，再由 sh_sample_quad 分别乘半宽 aw / 半高 ah），用于核对采点
 * 分布。idx 按行优先。 */
int sh_local_point(const ShRenderCfg *cfg, int idx, float *lx, float *ly);

/* ---- 第 2 段：按覆盖比例合成 ------------------------------------------------
 * 对每个像素追踪全部采样点，coverage = 未遮挡点数 / 总点数，写入 cov（可为
 * NULL）；亮度按 coverage 与距离衰减合成，写入 m。 */
int sh_composite(ShadowMap *m, float *cov, const ShLight *light,
                 const ShRenderCfg *cfg, const ShSeg *segs, int nseg);

/* ---- 第 3 段：半影宽度 ------------------------------------------------------
 * 扫描覆盖图，报告本影到全亮过渡带的平均/最大宽度。 */
int sh_penumbra(const float *cov, int w, int h, ShPenumbra *out);

/* 完整面光管线（内部依次调用上面三段；gear=OFF 或 aw=ah=0 时走旧点光源路径，
 * cov 全部写 0/1，out_pen 给出 0 宽度）。cov 可为 NULL。 */
int sh_render_ex(ShadowMap *m, float *cov, ShPenumbra *out_pen,
                 const ShLight *light, const ShRenderCfg *cfg,
                 const ShSeg *segs, int nseg);

#endif
