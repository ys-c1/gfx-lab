#ifndef ASSET_H
#define ASSET_H

#include <stddef.h>

#define AP_MAGIC "GPAK"
#define AP_VERSION_1 1u   /* 旧版：仅 name/size/offset */
#define AP_VERSION_2 2u   /* 新版：增加 flags/crc/时间戳/数据校验 */

/* ap_readx 选项位 */
#define AP_READ_MIGRATE 0x1u  /* 允许把 v1 旧包迁移补齐成 v2 结构；未置位时 v1 走拒绝路径 */

typedef struct {
    char name[32];
    unsigned int size;
    unsigned int offset;
    /* v2 新增（v1 迁移时由第二阶段补齐） */
    unsigned int flags;
    unsigned int crc;
} ApEntry;

typedef struct {
    ApEntry *entries;
    int count;
    unsigned int data_size;
    /* v2 新增 */
    unsigned int version;       /* 包的实际版本号 1 / 2 */
    unsigned int header_crc;    /* v2 头部 crc；v1 迁移时补齐 */
    unsigned long created_ms;   /* v2 创建时间戳；v1 迁移时补齐 */
    int migrated;               /* 1 = 由 v1 迁移而来 */
    int backfilled;             /* 第二阶段补齐的字段总数 */
} ApIndex;

/* 错误明细：任何失败都带上包内读到的版本号，便于定位 */
typedef struct {
    int code;            /* 0 成功；负数为错误码，见下 */
    unsigned int version;/* 从包中解析出的版本号；读不到时为 0 */
    char detail[128];
} ApError;

/* 错误码（与历史返回值保持一致） */
#define AP_OK            0
#define AP_ERR_PARAM    -1   /* 参数非法 */
#define AP_ERR_OPEN     -2   /* 文件打不开 */
#define AP_ERR_TRUNC    -3   /* 长度不足 / 字段缺失 / magic 错 */
#define AP_ERR_VERSION  -4   /* 版本未知，或迁移关闭时遇到旧版本 */
#define AP_ERR_NOTFOUND -5   /* 条目不存在 */
#define AP_ERR_CRC      -6   /* v2 校验和不匹配 */

int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size);
int ap_write_v1(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size);

/* 原接口保留：迁移关闭，旧包仍走原先的拒绝路径 */
int ap_read(const char *path, ApIndex *idx);
/* 带选项与错误明细的读取：第一阶段按版本号选分支，第二阶段补齐旧字段 */
int ap_readx(const char *path, unsigned int options, ApIndex *idx, ApError *err);

int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size);
int ap_extractx(const char *path, const char *name, unsigned int options,
                unsigned char **out, unsigned int *out_size, ApError *err);

void ap_free(ApIndex *idx);

/* IEEE CRC-32（与 Python zlib.crc32 一致），供校验与交叉核对使用 */
unsigned int ap_crc32(const unsigned char *buf, size_t len);
#endif
