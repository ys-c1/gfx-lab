#define _POSIX_C_SOURCE 200809L
/* 基准：同一批数据分别打成 v1/v2，比较读取误差、补齐字段数与耗时 */
#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_ENTRIES 2000
#define ENTRY_SZ  256
#define ROUNDS    50

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    const int n = N_ENTRIES;
    unsigned int total = (unsigned int)n * ENTRY_SZ;
    unsigned char *data = malloc(total);
    for (unsigned int i = 0; i < total; ++i) data[i] = (unsigned char)(i * 1103515245u >> 8);

    ApEntry *e = calloc(n, sizeof(ApEntry));
    for (int i = 0; i < n; ++i) {
        snprintf(e[i].name, sizeof(e[i].name), "asset_%04d.bin", i);
        e[i].size = ENTRY_SZ;
        e[i].offset = (unsigned int)i * ENTRY_SZ;
        e[i].flags = i & 0xff;
    }

    if (ap_write_v1("build/bench_v1.gpak", e, n, data, total) != 0) return 1;
    if (ap_write   ("build/bench_v2.gpak", e, n, data, total) != 0) return 1;

    ApIndex idx;
    ApError err;
    double t0, t1;

    /* v2 原生读取（无补齐） */
    t0 = now_sec();
    for (int r = 0; r < ROUNDS; ++r) {
        if (ap_readx("build/bench_v2.gpak", 0, &idx, &err) != 0) { printf("v2 read fail\n"); return 1; }
        if (r + 1 < ROUNDS) ap_free(&idx);
    }
    t1 = now_sec();
    printf("v2: version=%u count=%d migrated=%d backfilled=%d time_ms=%.3f\n",
           idx.version, idx.count, idx.migrated, idx.backfilled, (t1 - t0) * 1000.0 / ROUNDS);
    unsigned int v2_crc_sum = 0;
    for (int i = 0; i < n; ++i) v2_crc_sum += idx.entries[i].crc;
    ap_free(&idx);

    /* v1 迁移关闭：确认拒绝路径仍生效 */
    int rc = ap_readx("build/bench_v1.gpak", 0, &idx, &err);
    printf("v1 migrate-off -> rc=%d ver=%u (expected -4)\n", rc, err.version);

    /* v1 迁移读取（两阶段，含补齐） */
    t0 = now_sec();
    for (int r = 0; r < ROUNDS; ++r) {
        if (ap_readx("build/bench_v1.gpak", AP_READ_MIGRATE, &idx, &err) != 0) {
            printf("v1 migrate fail: %s\n", err.detail); return 1;
        }
        if (r + 1 < ROUNDS) ap_free(&idx);
    }
    t1 = now_sec();
    printf("v1: version=%u count=%d migrated=%d backfilled=%d time_ms=%.3f\n",
           idx.version, idx.count, idx.migrated, idx.backfilled, (t1 - t0) * 1000.0 / ROUNDS);
    unsigned int v1_crc_sum = 0;
    for (int i = 0; i < n; ++i) v1_crc_sum += idx.entries[i].crc;

    /* 读取误差核对：两档逐条比对 name/size/offset 与补齐的 crc */
    int mism = 0;
    ApIndex idx2;
    if (ap_readx("build/bench_v2.gpak", 0, &idx2, &err) != 0) return 1;
    for (int i = 0; i < n; ++i) {
        if (strcmp(idx.entries[i].name, idx2.entries[i].name) != 0 ||
            idx.entries[i].size != idx2.entries[i].size ||
            idx.entries[i].offset != idx2.entries[i].offset ||
            idx.entries[i].crc  != idx2.entries[i].crc) ++mism;
    }
    printf("cross-check: entry_mismatches=%d crc_sum_equal=%d\n", mism, v1_crc_sum == v2_crc_sum);
    ap_free(&idx);
    ap_free(&idx2);

    /* 提取耗时与误差：首尾两个条目，v1 迁移 vs v2 原生 */
    const char *names[2] = {"asset_0000.bin", "asset_1999.bin"};
    for (int k = 0; k < 2; ++k) {
        unsigned char *b1 = NULL, *b2 = NULL;
        unsigned int s1 = 0, s2 = 0;
        t0 = now_sec();
        for (int r = 0; r < ROUNDS; ++r) {
            free(b1); b1 = NULL;
            if (ap_extractx("build/bench_v1.gpak", names[k], AP_READ_MIGRATE, &b1, &s1, &err) != 0) return 1;
        }
        t1 = now_sec();
        double tv1 = (t1 - t0) * 1000.0 / ROUNDS;
        t0 = now_sec();
        for (int r = 0; r < ROUNDS; ++r) {
            free(b2); b2 = NULL;
            if (ap_extractx("build/bench_v2.gpak", names[k], 0, &b2, &s2, &err) != 0) return 1;
        }
        t1 = now_sec();
        double tv2 = (t1 - t0) * 1000.0 / ROUNDS;
        int byte_mismatch = (s1 != s2) || memcmp(b1, b2, s1) != 0;
        printf("extract %s: v1_ms=%.3f v2_ms=%.3f size_equal=%d byte_mismatch=%d\n",
               names[k], tv1, tv2, s1 == s2, byte_mismatch);
        free(b1); free(b2);
        if (byte_mismatch) { printf("BYTE MISMATCH\n"); return 1; }
    }

    free(data); free(e);
    printf("benchmark complete\n");
    return 0;
}
