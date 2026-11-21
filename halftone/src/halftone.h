#ifndef HALFTONE_H
#define HALFTONE_H

/* 半透明网点雾：两段式
 * 1) 网点判定：按 Bayer 阈值矩阵（逐像素平铺）决定该像素写不写网点；
 * 2) 覆盖率混色：覆盖率 c = 矩阵中低于该像素量化阈值的格点数 / n*n，
 *    对落在网点上的像素按 c 在底色与雾色之间做整数混色。
 * 不启用（enabled=0）时输出原样保留，不读取 alpha。
 */

typedef struct {
    int n;            /* 矩阵边长，2..16 且必须为 2 的幂 */
    unsigned char *m; /* n*n 个秩，0..n*n-1 的一个排列 */
    int *cover;       /* cover[q]：量化等级 q(0..255) 对应的网点格点数，长度 256 */
} HTMatrix;

typedef struct {
    int enabled;          /* 0 = 关闭，输出保持原样；非 0 = 启用 */
    unsigned char rgb[3]; /* 雾色 */
} HTFog;

/* 错误码：-1 非法参数（空指针/尺寸），-2 非法 alpha，-3 矩阵 n 非法，-4 文件错误 */
int ht_init(HTMatrix *ht, int n);
void ht_free(HTMatrix *ht);

/* 快速路径：cover 查找表 + 每像素一次矩阵索引。
 * rgb 为 w*h*3 的底图（原地输出），alpha 为 w*h、范围 [0,1]（仅启用时读取）。
 * 返回值 >=0 为写下的网点像素数；<0 为错误码，错误位置可用 ht_error() 查询。
 */
int ht_apply_fast(HTMatrix *ht, const HTFog *fog, unsigned char *rgb,
                  const float *alpha, int w, int h);

/* 暴力路径：每个像素遍历整个 n*n 矩阵统计格点数，不用查找表，结果与快速路径对照。 */
int ht_apply_brute(HTMatrix *ht, const HTFog *fog, unsigned char *rgb,
                   const float *alpha, int w, int h);

/* 独立的位交织 Bayer 构造（与 ht_init 的递推构造互相印证）。
 * 调用方提供 n*n 的输出缓冲；返回 0 成功，<0 错误码。
 */
int ht_bayer_brute(unsigned char *out, int n);

/* 矩阵导出：文本（人读，含全部秩值）与 PGM（秩值拉伸到 0..255）。 */
int ht_write_matrix_txt(const HTMatrix *ht, const char *path);
int ht_write_matrix_pgm(const HTMatrix *ht, const char *path);

/* 最近一次错误的位置与原因，例如：
 * "ht_apply_fast alpha[128]=1.5 out of range"；无错误时返回 NULL。 */
const char *ht_error(void);

#endif
