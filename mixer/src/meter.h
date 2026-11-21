#ifndef METER_H
#define METER_H

/*
 * 电平表（带表头阻尼的峰值表）
 *
 * 分两段实现，两个函数各自独立可测：
 *   第一段 meter_fall_alpha()：由表头阻尼参数（回落速度 fall_db_per_s，
 *       单位 dB/秒）和采样率，算出每采样阻尼系数 alpha。
 *   第二段 meter_update()：按该速度（alpha）逐点更新读数；攻击即时跟随，
 *       回落按指数阻尼衰减，且不会跌到真实信号之下。
 *
 * 旧行为（退化）：fall_db_per_s <= 0 时 alpha 取 0，读数逐点立即跟随
 * |x|，此时 meter 对整块给出的峰值与朴素 max|x| 完全一致。
 */

typedef struct {
    int    fs;               /* 采样率 Hz，必须 > 0 */
    float  fall_db_per_s;    /* 表头阻尼：回落速度 dB/s；<=0 表示无阻尼（旧行为） */
    float  dynamic_range_db; /* 动态范围 dB，必须 > 0，决定显示下限 */
    float  alpha;            /* 第一段算出的每采样阻尼系数 */
    float  floor_lin;        /* 动态范围对应的线性下限 10^(-DR/20) */
    float  peak;             /* 当前读数（线性幅度） */
} LevelMeter;

/* 错误码 */
#define METER_OK          0
#define METER_E_NULL     -1   /* 空指针 */
#define METER_E_EMPTY    -2   /* 空输入：frames <= 0，没有任何采样可测量 */
#define METER_E_PARAM    -3   /* 参数非法：采样率/动态范围非正 */

/* 初始化表头；fs、fall_db_per_s、dynamic_range_db 非法时返回错误码。
 * 初始化即完成第一段阻尼系数计算（alpha、floor_lin）。 */
int  meter_init(LevelMeter *m, int fs, float fall_db_per_s, float dynamic_range_db);

/* 第一段：由表头阻尼（回落速度 dB/s）计算每采样阻尼系数。
 * fall_db_per_s <= 0 时返回 0（旧行为）。fs 非法返回负数错误码。 */
int  meter_fall_alpha(int fs, float fall_db_per_s, float *out_alpha);

/* 单采样更新（第二段的核心一步），返回更新后的线性读数。 */
float meter_update(LevelMeter *m, float x);

/* 对一整块采样跑表，返回块峰值（线性）；frames<=0 或 src 为 NULL 报错。
 * 表内状态会被保留并继续推进（peak 不重置）。 */
int  meter_process(LevelMeter *m, const float *src, int frames, float *out_peak);

/* 把读数曲线导出为 CSV：sample,t_s,target_lin,peak_lin,peak_db。
 * step>1 时每 step 个采样导出一行（含首尾）。返回写入行数或负错误码。 */
int  meter_export_curve(const LevelMeter *m, const float *src, int frames,
                        int step, const char *path);

/* 当前读数的 dB 值；读数为 0 时返回 -infinity。 */
float meter_peak_db(const LevelMeter *m);

/* 错误码 -> 中文原因说明。 */
const char *meter_strerror(int err);

#endif
