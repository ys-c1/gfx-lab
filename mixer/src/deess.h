#ifndef DEESS_H
#define DEESS_H
/* 三段式齿音抑制 (de-esser):
 *   段1 按频段检出齿音: 齿音带(biquad BPF) RMS 包络 / 全带 RMS 包络, 比值越限且带电平门限
 *   段2 按强度算衰减量: att_db = min(max_att_db, slope * 20*log10(peak_ratio/thresh))
 *   段3 逐段套用: 每段梯形增益(attack 下压 / 保持 / release 恢复), 只压齿音带
 * enabled == 0 时输出与输入逐位一致(纯 memcpy)。 */
typedef struct {
    int enabled;                /* 0 = 旁通(逐位一致), 1 = 处理 */
    int sample_rate;            /* > 0 */
    float band_lo_hz;           /* 齿音带下沿, > 0 */
    float band_hi_hz;           /* 齿音带上沿, > band_lo_hz 且 < sample_rate/2 */
    float thresh_ratio;         /* 带/全带比值门限, (0,1) */
    float min_level;            /* 带包络绝对门限(线性), > 0 */
    float max_att_db;           /* 衰减上限 dB, > 0 */
    float slope;                /* 每 dB 超出量折算的衰减 dB, > 0 */
    float detect_ms;            /* 检出 RMS 包络时间常数, > 0 */
    float attack_ms;            /* 段首增益下压斜坡, > 0 */
    float release_ms;           /* 段尾增益恢复斜坡, > 0 */
    float hang_ms;              /* 段尾保持, >= 0 */
    float merge_gap_ms;         /* 小于该间隔的相邻段合并, >= 0 */
    float min_seg_ms;           /* 短于该时长的段丢弃, >= 0 */
} DsCfg;

typedef struct {
    int channel;
    int start_frame, end_frame; /* [start, end) */
    float att_db;               /* 本段套用的衰减量 */
    float peak_ratio;           /* 段内带/全带峰值比 */
} DsSeg;

typedef struct {
    int segments;               /* 写入 segs 数组的段数 */
    int segments_total;         /* 实际检出的段数(可能超过数组容量) */
    float max_att_db;           /* 所有段中的最大衰减量 */
    float peak_in, peak_out;    /* 输入/输出峰值(全声道) */
} DsStats;

typedef struct DsCtx DsCtx;

DsCfg ds_default_cfg(int sample_rate);
/* 校验配置, 返回冲突项数; errs 逐条列出冲突(每条最多 95 字符) */
int ds_validate(const DsCfg *c, char errs[][96], int max_errs);
/* enabled==0 时不校验参数, 直接建立旁通上下文; 否则校验失败返回 NULL */
DsCtx *ds_create(const DsCfg *c, int channels);
void ds_destroy(DsCtx *d);
/* in/out 为交错采样, frames 为每声道帧数; 滤波状态跨调用保持(流式) */
int ds_process(DsCtx *d, const float *in, float *out, int frames,
               DsSeg *segs, int max_segs, DsStats *stats);
/* 导出上一次 ds_process 的齿音分布: frame,ch,band_env,full_env,ratio,gain */
int ds_export_csv(const DsCtx *d, const char *path);
#endif
