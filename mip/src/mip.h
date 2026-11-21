#ifndef MIP_H
#define MIP_H

#include <stddef.h>

typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;

int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);

/* ---- 分两段:按层采样缩略图 -> 按层拼成预览图 ---- */

/* 错误码:每个码对应一个字段名;MipError.index 指出出错的层号或数组下标 */
enum {
    MIP_OK = 0,
    MIP_E_ARG = -1,   /* 空指针 / 一般参数非法 */
    MIP_E_W = -2,     /* 缩略图或预览图尺寸非法(<=0 或溢出) */
    MIP_E_COUNT = -3, /* 每轴采样次数非法(< 1 或 > 256) */
    MIP_E_LEVEL = -4, /* 层号越界(需 0 <= level < levels) */
    MIP_E_ORDER = -5, /* 缩略图未按层号严格升序:重复、乱序或跨链 */
    MIP_E_SHEET = -6, /* 预览图布局参数非法(负间距) */
    MIP_E_THUMB = -7  /* 缩略图未初始化、尺寸非法或来自别的层链 */
};

typedef struct {
    int code;          /* MIP_OK 或上面的错误码 */
    const char *field; /* 出错字段名:"level"/"width"/"samples"/"order"/"sheet"/"thumb" */
    int index;         /* 出错的层号;若错误在数组位置则为其下标;-1 表示不适用 */
} MipError;

/* 字段名查询:rc == 0 返回 NULL */
const char *mip_err_field(int rc);

/* 段1 输入:采样哪一层、缩略图多大、每轴采样几个点。
 * 每像素被切成 samples x samples 个格子,每格取中心所在纹素后求平均。 */
typedef struct {
    int level;    /* 层号,0 <= level < levels */
    int tw, th;   /* 缩略图尺寸,均 > 0 */
    int samples;  /* 每轴采样点数,1..256;1 为默认(每像素 1 次读取) */
} MipThumbSpec;

/* 段1 输出:缩略图形状与统计(pixels 由调用方 free) */
typedef struct {
    int level;         /* 取自 spec->level,段2据此排序 */
    int w, h;          /* 缩略图尺寸 */
    int samples;       /* 实际使用的每轴采样点数 */
    unsigned long long sample_reads; /* 实际纹素读取次数 = samples^2 * w * h */
    const MipChain *chain;           /* 采样来源链,段2据此拒绝跨链混拼 */
    unsigned char *pixels;           /* w*h*3 */
} MipThumb;

/* 段2 摆放参数;pad 为层间距,label_gap 为每层上方预留的层号标签高度(不绘制文字) */
typedef struct {
    int pad;
    int label_gap;
} MipSheetSpec;

/* 段2 输出:预览图与层间差异统计(pixels / x / y / tw / th 由调用方 free) */
typedef struct {
    int w, h;             /* 预览图尺寸 */
    int cols;             /* 网格列数 */
    int count;            /* 层数 */
    int *x, *y;           /* 每层缩略图在预览图中的左上角 */
    int *tw, *th;         /* 每层缩略图宽高 */
    unsigned long long inter_layer_bytes; /* 相邻层公共区域逐字节绝对差的累加 */
    unsigned long long inter_layer_px;    /* 参与比较的通道数(3 通道合计) */
    double inter_layer_mean;              /* inter_layer_bytes / inter_layer_px */
    unsigned char *pixels;                /* w*h*3 */
} MipSheet;

/* 默认:缩略图取整层原尺寸,每像素 1 次采样,pad=0,label_gap=0。
 * 此时段1输出逐字节等于 c->pix[level],即"恢复默认=当前版本"。 */
#define MIP_THUMB_SPEC_DEFAULT(level_, c_) \
    { (level_), (c_)->w[(level_)], (c_)->h[(level_)], 1 }
#define MIP_SHEET_SPEC_DEFAULT { 0, 0 }

/* 段1:按层号单独采样。level/w/h/samples 非法时返回对应错误码并填 err(可 NULL)。
 * 成功返回 MIP_OK 并填满 out(其 pixels 需调用方 free)。 */
int mip_thumb_level(const MipChain *c, const MipThumbSpec *spec,
                    MipThumb *out, MipError *err);

/* 段2:把若干缩略图按层号拼成一张预览图,并单独统计相邻层的差异。
 * 缩略图须按 thumbs[0..n-1].level 严格升序且无重复,且都来自同一条链 c,
 * 否则返回 MIP_E_ORDER,并在 err->index 指出第一个出错的数组位置。
 * 布局为近似居中的网格(cols = ceil(sqrt(n))),格宽取各缩略图最大宽,
 * 格高取最大高加 label_gap。层间差异在同一层号坐标的公共区域(取两者较小的
 * 宽高)上逐通道比较,单独累计,不混入像素总数。 */
int mip_sheet_assemble(const MipChain *c, const MipThumb *thumbs, int n,
                       const MipSheetSpec *spec, MipSheet *out, MipError *err);

/* 把 RGB 缓冲写成二进制 PPM(P6),供预览图/缩略图落盘查看。成功返回 MIP_OK。 */
int mip_write_ppm(const unsigned char *pixels, int w, int h, const char *path);

#endif
