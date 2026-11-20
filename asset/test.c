#include "asset.h"
#include "apack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 现状布局的独立参考实现,用于逐字节对照 */
static void ref_layout(const ApBlock *b, int n, unsigned int *off, unsigned int *total) {
    unsigned int cur = 0;
    for (int i = 0; i < n; ++i) {
        unsigned int m = b[i].align - 1;
        cur = (cur + m) & ~m;
        off[i] = cur;
        cur += b[i].size;
    }
    *total = cur;
}

static unsigned long long t_state = 0x123456789abcdefULL;
static unsigned int t_rnd(void) {
    t_state = t_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned int)(t_state >> 33);
}

typedef struct { unsigned int off, size; } Span;
static int cmp_span(const void *pa, const void *pb) {
    const Span *a = (const Span *)pa, *b = (const Span *)pb;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return 0;
}

static long read_file(const char *path, unsigned char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = (unsigned char *)malloc(sz ? sz : 1);
    if (*out && sz) { size_t got = fread(*out, 1, sz, f); (void)got; }
    fclose(f);
    return sz;
}

static void pack_tests(void) {
    /* 1. 校验:非法对齐、零块大小,报错并指出字段 */
    unsigned int off[4]; unsigned int tot; ApPackError err;
    ApBlock bad_align[2] = {{4, 4}, {8, 3}};
    CK(ap_pack_plan(bad_align, 2, AP_PACK_TIGHT, off, &tot, &err) == AP_PACK_ERR_ALIGN);
    CK(err.index == 1 && strcmp(err.field, "align") == 0);
    ApBlock zero_align[1] = {{4, 0}};
    CK(ap_pack_plan(zero_align, 1, AP_PACK_TIGHT, off, &tot, &err) == AP_PACK_ERR_ALIGN);
    CK(err.index == 0 && strcmp(err.field, "align") == 0);
    ApBlock zero_size[2] = {{4, 4}, {0, 2}};
    CK(ap_pack_plan(zero_size, 2, AP_PACK_TIGHT, off, &tot, &err) == AP_PACK_ERR_SIZE);
    CK(err.index == 1 && strcmp(err.field, "size") == 0);
    CK(ap_pack_plan(NULL, 1, 0, off, &tot, NULL) == AP_PACK_ERR_ARG);
    CK(ap_pack_plan(NULL, 0, 0, NULL, &tot, NULL) == AP_PACK_OK && tot == 0);

    /* 2. 关闭优化时与参考布局逐字节一致(偏移、总跨度、包体字节) */
    ApBlock pb[5] = {{8, 4}, {5, 1}, {16, 16}, {3, 2}, {64, 8}};
    unsigned int roff[5], rtot, poff[5], ptot;
    ref_layout(pb, 5, roff, &rtot);
    CK(ap_pack_plan(pb, 5, AP_PACK_GREEDY, poff, &ptot, NULL) == AP_PACK_OK);
    CK(rtot == ptot && memcmp(roff, poff, sizeof roff) == 0);
    {
        ApEntry e1[5], e2[5];
        unsigned char data[64];
        for (int i = 0; i < 64; ++i) data[i] = (unsigned char)(i * 7 + 1);
        memset(e1, 0, sizeof e1); memset(e2, 0, sizeof e2);
        for (int i = 0; i < 5; ++i) {
            sprintf(e1[i].name, "p%d", i); e1[i].size = pb[i].size; e1[i].offset = roff[i];
            sprintf(e2[i].name, "p%d", i); e2[i].size = pb[i].size;
        }
        ap_pack_apply(e2, poff, 5);
        CK(ap_write("/tmp/pk_a.gpak", e1, 5, data, rtot) == 0);
        CK(ap_write("/tmp/pk_b.gpak", e2, 5, data, ptot) == 0);
        unsigned char *ba = NULL, *bb = NULL;
        long sa = read_file("/tmp/pk_a.gpak", &ba);
        long sb = read_file("/tmp/pk_b.gpak", &bb);
        CK(sa == sb && sa > 0 && memcmp(ba, bb, sa) == 0);
        free(ba); free(bb);
    }

    /* 3. 紧凑布局不变量(随机实例):对齐达标、互不重叠、不劣于贪心、确定性 */
    {
        static const unsigned int aligns[7] = {1, 2, 4, 8, 16, 32, 64};
        for (int t = 0; t < 40; ++t) {
            int n = 1 + (int)(t_rnd() % 200);
            ApBlock *b = (ApBlock *)malloc(n * sizeof *b);
            unsigned int *og = (unsigned int *)malloc(n * sizeof *og);
            unsigned int *ot = (unsigned int *)malloc(n * sizeof *ot);
            unsigned int *ot2 = (unsigned int *)malloc(n * sizeof *ot2);
            Span *sp = (Span *)malloc(n * sizeof *sp);
            unsigned long long sum = 0;
            for (int i = 0; i < n; ++i) {
                b[i].size = 1 + t_rnd() % 500;
                b[i].align = aligns[t_rnd() % 7];
                sum += b[i].size;
            }
            unsigned int tg, tt, tt2;
            CK(ap_pack_plan(b, n, AP_PACK_GREEDY, og, &tg, NULL) == AP_PACK_OK);
            CK(ap_pack_plan(b, n, AP_PACK_TIGHT, ot, &tt, NULL) == AP_PACK_OK);
            CK(ap_pack_plan(b, n, AP_PACK_TIGHT, ot2, &tt2, NULL) == AP_PACK_OK);
            CK(tt == tt2 && memcmp(ot, ot2, n * sizeof *ot) == 0); /* 确定性 */
            CK(tt <= tg);                                          /* 不劣于贪心 */
            CK(tt >= sum);                                         /* 不小于数据量 */
            int aligned = 1;
            for (int i = 0; i < n; ++i) {
                if (ot[i] % b[i].align != 0) aligned = 0;
                sp[i].off = ot[i]; sp[i].size = b[i].size;
            }
            CK(aligned);
            qsort(sp, n, sizeof *sp, cmp_span);
            int disjoint = 1;
            for (int i = 1; i < n; ++i)
                if (sp[i].off < sp[i - 1].off + sp[i - 1].size) disjoint = 0;
            CK(disjoint);
            CK(sp[n - 1].off + sp[n - 1].size <= tt);
            free(b); free(og); free(ot); free(ot2); free(sp);
        }
    }

    /* 4. 回填索引后写包、读回、解包,内容逐字节正确 */
    {
        ApBlock rb[3] = {{10, 4}, {20, 8}, {5, 1}};
        const char *names[3] = {"x.bin", "y.bin", "z.bin"};
        unsigned char content[64];
        for (int i = 0; i < 64; ++i) content[i] = (unsigned char)(i * 5 + 3);
        unsigned int ro[3], rt;
        CK(ap_pack_plan(rb, 3, AP_PACK_TIGHT, ro, &rt, NULL) == AP_PACK_OK);
        ApEntry re[3];
        memset(re, 0, sizeof re);
        for (int i = 0; i < 3; ++i) { strcpy(re[i].name, names[i]); re[i].size = rb[i].size; }
        ap_pack_apply(re, ro, 3);
        CK(ap_write("/tmp/pk_r.gpak", re, 3, content, rt) == 0);
        for (int i = 0; i < 3; ++i) {
            unsigned char *buf = NULL; unsigned int sz = 0;
            CK(ap_extract("/tmp/pk_r.gpak", names[i], &buf, &sz) == 0);
            CK(sz == rb[i].size && memcmp(buf, content + ro[i], sz) == 0);
            free(buf);
        }
    }
}

int main(void) {
    unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "a.bin"); e[0].size = 4; e[0].offset = 0;
    strcpy(e[1].name, "b.bin"); e[1].size = 4; e[1].offset = 4;
    CK(ap_write("/tmp/t.gpak", e, 2, data, 8) == 0);
    ApIndex idx;
    CK(ap_read("/tmp/t.gpak", &idx) == 0);
    CK(idx.count == 2 && idx.data_size == 8);
    CK(strcmp(idx.entries[1].name, "b.bin") == 0);
    ap_free(&idx);
    unsigned char *buf = NULL; unsigned int sz = 0;
    CK(ap_extract("/tmp/t.gpak", "b.bin", &buf, &sz) == 0);
    CK(sz == 4 && buf[0] == 5);
    free(buf);
    CK(ap_extract("/tmp/t.gpak", "zzz", &buf, &sz) == -5);
    CK(ap_write("/tmp/bad.gpak", e, -1, data, 8) == -1);
    pack_tests();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
