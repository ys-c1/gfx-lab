#define _POSIX_C_SOURCE 199309L
#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static unsigned long long fsize(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s < 0 ? 0 : (unsigned long long)s;
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int eq = 1;
    for (;;) { int ca = fgetc(fa), cb = fgetc(fb); if (ca != cb) { eq = 0; break; } if (ca == EOF) break; }
    fclose(fa); fclose(fb);
    return eq;
}

/* 生成 n 个条目：n/2 份不同资源，每份在数据里放两遍（模拟重复打包） */
static void gen(int n, ApEntry **out_e, unsigned char **out_d, unsigned int *out_ds) {
    int uniq = n / 2;
    ApEntry *e = (ApEntry*)calloc((size_t)n, sizeof(*e));
    unsigned int ds = 0;
    for (int k = 0; k < uniq; ++k) ds += 2u * (32u + (unsigned int)(k * 37) % 224u);
    unsigned char *d = (unsigned char*)malloc(ds);
    unsigned int off = 0;
    for (int k = 0; k < uniq; ++k) {
        unsigned int sz = 32u + (unsigned int)(k * 37) % 224u;
        for (unsigned int i = 0; i < sz; ++i)
            d[off + i] = (unsigned char)(k * 131 + (int)i * 7 + ((int)i >> 3));
        snprintf(e[2 * k].name, 32, "r%05d_a", k);
        e[2 * k].size = sz; e[2 * k].offset = off;
        off += sz;
        memcpy(d + off, d + e[2 * k].offset, sz); /* 同一份资源再放一遍 */
        snprintf(e[2 * k + 1].name, 32, "r%05d_b", k);
        e[2 * k + 1].size = sz; e[2 * k + 1].offset = off;
        off += sz;
    }
    *out_e = e; *out_d = d; *out_ds = ds;
}

/* 逐条手比基准：读回索引后逐条 fseek/fread，与原始字节 memcmp；返回不一致条数 */
static int verify(const char *path, const ApEntry *src, int n, const unsigned char *data) {
    ApIndex idx;
    if (ap_read(path, &idx) != 0) return -1;
    if (idx.count != n) { ap_free(&idx); return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) { ap_free(&idx); return -1; }
    unsigned int header = 4 + 2 + 2 + (unsigned int)idx.count * 40 + 4;
    unsigned char buf[256];
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        if (strncmp(idx.entries[i].name, src[i].name, 31) != 0) { ++bad; continue; }
        if (idx.entries[i].size != src[i].size || src[i].size > sizeof(buf)) { ++bad; continue; }
        fseek(f, (long)(header + idx.entries[i].offset), SEEK_SET);
        if (fread(buf, 1, src[i].size, f) != src[i].size) { ++bad; continue; }
        if (memcmp(buf, data + src[i].offset, src[i].size) != 0) ++bad;
    }
    fclose(f);
    ap_free(&idx);
    return bad;
}

int main(void) {
    const int scales[] = {1000, 10000};
    for (int s = 0; s < 2; ++s) {
        int n = scales[s], err = -1;
        ApEntry *e; unsigned char *d; unsigned int ds;
        gen(n, &e, &d, &ds);

        double t0 = now_ms();
        int rc0 = ap_pack("build/bench_off.gpak", e, n, d, ds, 0, NULL, &err);
        double t1 = now_ms();
        int rc1 = ap_pack("build/bench_on.gpak", e, n, d, ds, 1, NULL, &err);
        double t2 = now_ms();
        if (rc0 != 0 || rc1 != 0) { printf("N=%d 打包失败 rc=%d/%d err=%d\n", n, rc0, rc1, err); return 1; }

        /* 对照：dedup=0 必须与 ap_write 直写逐字节一致 */
        if (ap_write("build/bench_ref.gpak", e, n, d, ds) != 0) return 1;
        int identical = files_equal("build/bench_off.gpak", "build/bench_ref.gpak");

        double t3 = now_ms();
        int bad_off = verify("build/bench_off.gpak", e, n, d);
        int bad_on = verify("build/bench_on.gpak", e, n, d);
        double t4 = now_ms();

        unsigned long long sz_off = fsize("build/bench_off.gpak");
        unsigned long long sz_on = fsize("build/bench_on.gpak");
        unsigned long long saved = sz_off - sz_on;
        printf("N=%d 条目（每份资源存两遍）\n", n);
        printf("  体积: 关闭去重 %llu B, 开启去重 %llu B, 节省 %llu B (%.1f%%)\n",
               sz_off, sz_on, saved, sz_off ? 100.0 * (double)saved / (double)sz_off : 0.0);
        printf("  读取误差（逐条手比）: 关闭去重 %d/%d 条不一致, 开启去重 %d/%d 条不一致\n",
               bad_off, n, bad_on, n);
        printf("  耗时: 写入 关闭 %.2f ms / 开启 %.2f ms, 逐条校验 %.2f ms\n",
               t1 - t0, t2 - t1, t4 - t3);
        printf("  dedup=0 与 ap_write 输出逐字节一致: %s\n", identical ? "是" : "否");
        free(e); free(d);
        if (bad_off || bad_on || !identical) return 1;
    }
    return 0;
}
