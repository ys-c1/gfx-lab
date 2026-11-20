#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define TDIR "build/t"
#define P(n) TDIR "/" n

static void make_entries(ApEntry *e) {
    memset(e, 0, 2 * sizeof(ApEntry));
    strcpy(e[0].name, "a.bin"); e[0].size = 4; e[0].offset = 0; e[0].flags = 0x11;
    strcpy(e[1].name, "b.bin"); e[1].size = 4; e[1].offset = 4; e[1].flags = 0x22;
}

static void write_file(const char *path, const unsigned char *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(bytes, 1, n, f);
    fclose(f);
}

int main(void) {
    mkdir(TDIR, 0755);
    unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ApEntry e[2];
    make_entries(e);

    /* ---- v2 基础往返 ---- */
    CK(ap_write(P("t2.gpak"), e, 2, data, 8) == 0);
    ApIndex idx;
    ApError err;
    CK(ap_readx(P("t2.gpak"), 0, &idx, &err) == 0);
    CK(idx.version == 2 && idx.count == 2 && idx.data_size == 8);
    CK(idx.migrated == 0 && idx.backfilled == 0);
    CK(idx.entries[1].flags == 0x22 && idx.entries[1].crc == ap_crc32(data + 4, 4));
    CK(idx.header_crc != 0 && idx.created_ms != 0);
    ap_free(&idx);

    unsigned char *buf = NULL; unsigned int sz = 0;
    CK(ap_extract(P("t2.gpak"), "b.bin", &buf, &sz) == 0);
    CK(sz == 4 && buf[0] == 5 && buf[3] == 8);
    free(buf); buf = NULL;
    CK(ap_extract(P("t2.gpak"), "zzz", &buf, &sz) == AP_ERR_NOTFOUND);
    CK(buf == NULL);

    /* ---- v1 旧包：迁移关闭走原先的拒绝路径 ---- */
    CK(ap_write_v1(P("t1.gpak"), e, 2, data, 8) == 0);
    CK(ap_read(P("t1.gpak"), &idx) == AP_ERR_VERSION);          /* 历史接口：拒绝 */
    CK(ap_readx(P("t1.gpak"), 0, &idx, &err) == AP_ERR_VERSION);
    CK(err.version == 1);                                          /* 错误带版本号 */
    CK(strstr(err.detail, "v1") != NULL);

    /* ---- v1 旧包：迁移开启，两阶段补齐为 v2 结构 ---- */
    CK(ap_readx(P("t1.gpak"), AP_READ_MIGRATE, &idx, &err) == 0);
    CK(idx.version == 1 && idx.migrated == 1);
    CK(idx.backfilled == 2 + 2 * 2);                              /* 头 2 + 每条目 2 */
    CK(idx.entries[0].flags == 0 && idx.entries[1].flags == 0);    /* 补齐默认值 */
    CK(idx.entries[0].crc == ap_crc32(data, 4));                   /* 补齐真实 crc */
    CK(idx.entries[1].crc == ap_crc32(data + 4, 4));
    CK(idx.header_crc == 0 && idx.created_ms == 0);                /* 无从重建的留 0 */
    CK(strcmp(idx.entries[1].name, "b.bin") == 0 && idx.entries[1].size == 4);
    ap_free(&idx);

    /* 迁移后的提取：走补齐的 crc 核验，数据与 v2 一致 */
    CK(ap_extractx(P("t1.gpak"), "a.bin", AP_READ_MIGRATE, &buf, &sz, &err) == 0);
    CK(sz == 4 && buf[0] == 1);
    free(buf); buf = NULL;
    /* 迁移关闭时提取旧包同样被拒绝 */
    CK(ap_extract(P("t1.gpak"), "a.bin", &buf, &sz) == AP_ERR_VERSION);
    CK(buf == NULL);

    /* ---- 未知版本：报错并指出版本 ---- */
    {
        unsigned char raw[16];
        memcpy(raw, "GPAK", 4);
        raw[4] = 9; raw[5] = 0; raw[6] = 0; raw[7] = 0;   /* version = 9 */
        memset(raw + 8, 0, 8);
        write_file(P("badver.gpak"), raw, sizeof(raw));
        CK(ap_readx(P("badver.gpak"), AP_READ_MIGRATE, &idx, &err) == AP_ERR_VERSION);
        CK(err.version == 9);
        CK(strstr(err.detail, "version 9") != NULL);
    }

    /* ---- 长度不足：连版本头都读不完 ---- */
    {
        write_file(P("short.gpak"), (const unsigned char *)"GPAK\x01", 5);
        CK(ap_readx(P("short.gpak"), AP_READ_MIGRATE, &idx, &err) == AP_ERR_TRUNC);
        CK(strstr(err.detail, "length 5") != NULL);
    }

    /* ---- 字段缺失：声明 2 个条目但只放 1 个 ---- */
    {
        FILE *f = fopen(P("missing.gpak"), "wb");
        unsigned char hdr[8] = {'G','P','A','K', 1,0, 2,0};      /* v1, count=2 */
        fwrite(hdr, 1, 8, f);
        char name[32] = "only.bin";
        fwrite(name, 1, 32, f);
        unsigned int v = 4; fwrite(&v, 4, 1, f);
        fwrite(&v, 4, 1, f);                                     /* 仅 1 个完整条目 */
        fclose(f);
        CK(ap_readx(P("missing.gpak"), AP_READ_MIGRATE, &idx, &err) == AP_ERR_TRUNC);
        CK(err.version == 1);                                    /* 错误指出版本 */
        CK(strstr(err.detail, "v1") != NULL);
    }

    /* ---- v2 缺字段：header_crc / created_ms 分别截断 ---- */
    {
        unsigned char raw[64];
        memcpy(raw, "GPAK", 4);
        raw[4] = 2; raw[5] = 0; raw[6] = 0; raw[7] = 0;          /* v2, count=0 */
        memset(raw + 8, 0, sizeof(raw) - 8);
        write_file(P("v2short.gpak"), raw, 14);                  /* header_crc 只给 2 字节 */
        CK(ap_readx(P("v2short.gpak"), 0, &idx, &err) == AP_ERR_TRUNC);
        CK(err.version == 2);
        CK(strstr(err.detail, "header_crc") != NULL);
        write_file(P("v2short2.gpak"), raw, 20);                 /* header_crc 完整，缺 created_ms */
        CK(ap_readx(P("v2short2.gpak"), 0, &idx, &err) == AP_ERR_TRUNC);
        CK(err.version == 2 && strstr(err.detail, "created_ms") != NULL);
    }

    /* ---- 数据区截断 ---- */
    {
        FILE *f = fopen(P("trunc.gpak"), "wb");
        unsigned char hdr[8] = {'G','P','A','K', 1,0, 1,0};
        fwrite(hdr, 1, 8, f);
        char name[32] = "x.bin"; fwrite(name, 1, 32, f);
        unsigned int size = 100, off = 0;                        /* 声明 100 字节 */
        fwrite(&size, 4, 1, f); fwrite(&off, 4, 1, f);
        unsigned int dsz = 100; fwrite(&dsz, 4, 1, f);
        unsigned char payload[10] = {0}; fwrite(payload, 1, 10, f);  /* 只给 10 */
        fclose(f);
        CK(ap_readx(P("trunc.gpak"), AP_READ_MIGRATE, &idx, &err) == AP_ERR_TRUNC);
        CK(err.version == 1);
    }

    /* ---- bad magic ---- */
    {
        write_file(P("magic.gpak"), (const unsigned char *)"XXXX\x01\x00\x00\x00", 8);
        CK(ap_readx(P("magic.gpak"), AP_READ_MIGRATE, &idx, &err) == AP_ERR_TRUNC);
        CK(strstr(err.detail, "magic") != NULL);
    }

    /* ---- v2 数据损坏：crc 报错 ---- */
    {
        FILE *src = fopen(P("t2.gpak"), "rb");
        fseek(src, 0, SEEK_END); long n = ftell(src); rewind(src);
        unsigned char *raw = malloc((size_t)n);
        if (fread(raw, 1, (size_t)n, src) != (size_t)n) { fclose(src); free(raw); return 2; }
        fclose(src);
        raw[n - 5] ^= 0xff;                                      /* 破坏数据区末字节 */
        write_file(P("corrupt.gpak"), raw, (size_t)n);
        free(raw);
        CK(ap_readx(P("corrupt.gpak"), 0, &idx, &err) == AP_ERR_CRC);
        CK(err.version == 2);
        CK(strstr(err.detail, "data_crc") != NULL);
    }

    /* ---- 参数校验 ---- */
    CK(ap_write_v1(P("x.gpak"), e, -1, data, 8) == AP_ERR_PARAM);
    CK(ap_readx(NULL, 0, &idx, &err) == AP_ERR_PARAM);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
