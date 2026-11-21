#define _POSIX_C_SOURCE 199309L
#include "atlas_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !ATLAS_USE_VOL

int main(void) {
    printf("atlas_vol demo: disabled (build with -DATLAS_USE_VOL=1)\n");
    return 0;
}

#else

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* got / paper 并排打印：纸笔值为 0 的项（误差/浪费）单独标 OK/MISMATCH。 */
static void cmp_i(const char *name, long got, long paper) {
    printf("  %-22s got=%-10ld paper=%-10ld %s\n",
           name, got, paper, got == paper ? "OK" : "MISMATCH");
}
static void cmp_d(const char *name, double got, double paper) {
    double d = got - paper;
    if (d < 0) d = -d;
    printf("  %-22s got=%-10.4f paper=%-10.4f %s\n",
           name, got, paper, d < 1e-9 ? "OK" : "MISMATCH");
}

/* 手算模型 A：8 个 32x16 块，每册 64x32、6144 字节/册、RGB。
 *   1x：每册 4 块（512*4=2048 点，正好铺满），2 册；
 *   2x：每块 512*4=2048 物理像素*3=6144 字节，一册只装 1 块，8 册。 */
static void case_hand(int scale) {
    AtlasRect r[8];
    for (int i = 0; i < 8; ++i) { r[i].w = 32; r[i].h = 16; }
    AvolLimits lim = {64, 32, 6144, 3, scale};
    AvolBook b;
    AvolStats st;
    int err = -1;
    double t0 = now_ms();
    int rc = avol_pack(r, 8, &lim, &b, &st, &err, 1);
    double dt = now_ms() - t0;
    if (rc != AVOL_OK) {
        printf("scale=%d pack failed rc=%d err_item=%d\n", scale, rc, err);
        return;
    }
    int bad = -1;
    long parsed = 0;
    int ok = avol_validate(&b, r, &bad) == AVOL_OK &&
             avol_probe(&b, r, &parsed, &bad) == AVOL_OK;

    printf("\n== 手算算例 A：8 x 32x16，每册 64x32，scale=%dx，pack %.3f ms ==\n",
           scale, dt);
    printf("  volumes=%d index_len=%d index_bytes=%d samples=%ld occupancy=%.4f\n",
           st.volumes, st.index_len, st.index_bytes, st.samples, st.occupancy);
    printf("  validate+probe: %s (parsed=%ld, oob_index=%d)\n",
           ok ? "OK" : "FAIL", parsed, bad);
    for (int p = 0; p < b.volume_count; ++p)
        printf("  vol %d: rects=%d used_px=%d used_bytes=%ld/%ld shelves=%d occupancy=%.4f\n",
               p, b.vols[p].rects, b.vols[p].used_px, b.vols[p].used_bytes,
               lim.cap_bytes, b.vols[p].shelves, b.vols[p].occupancy);

    if (scale == 1) {
        cmp_i("volumes", st.volumes, 2);
        cmp_i("index_len", st.index_len, 10);
        cmp_i("index_bytes", st.index_bytes, 16 + 12 * 8 + 20 * 2);
        cmp_i("samples", st.samples, 4096);
        cmp_i("used_px", st.used_px, 4096);
        cmp_i("used_bytes", st.used_bytes, 12288);
        cmp_i("waste_px", st.waste_px, 0);
        cmp_i("waste_bytes", st.waste_bytes, 0);
        cmp_d("occupancy", st.occupancy, 1.0);
        for (int p = 0; p < 2; ++p) {
            cmp_i("per-vol used_bytes", b.vols[p].used_bytes, 6144);
            cmp_i("per-vol rects", b.vols[p].rects, 4);
        }
    } else {
        cmp_i("volumes", st.volumes, 8);
        cmp_i("index_len", st.index_len, 16);
        cmp_i("index_bytes", st.index_bytes, 16 + 12 * 8 + 20 * 8);
        cmp_i("samples", st.samples, 16384);
        cmp_i("used_px", st.used_px, 4096);
        cmp_i("used_bytes", st.used_bytes, 49152);
        cmp_i("waste_px", st.waste_px, 8 * 2048 - 4096);
        cmp_i("waste_bytes", st.waste_bytes, (8 * 2048 - 4096) * 4 * 3);
        cmp_d("occupancy", st.occupancy, 4096.0 / (8.0 * 64 * 32));
        for (int p = 0; p < 8; ++p) {
            cmp_i("per-vol used_bytes", b.vols[p].used_bytes, 6144);
            cmp_i("per-vol rects", b.vols[p].rects, 1);
        }
    }

    FILE *f = fopen(scale == 1 ? "demo_vol_1x.txt" : "demo_vol_2x.txt", "w");
    if (f) {
        if (avol_export(&b, r, f) == AVOL_OK) printf("  page table -> %s\n",
            scale == 1 ? "demo_vol_1x.txt" : "demo_vol_2x.txt");
        fclose(f);
    }
    avol_free(&b);
}

/* 压力算例：64 个随机矩形，两档倍率下报册数/每册占用/索引长度/采样次数/耗时。 */
static void case_stress(int scale) {
    enum { N = 64 };
    static AtlasRect r[N];
    unsigned s = 7u;
    for (int i = 0; i < N; ++i) {
        s = s * 1103515245u + 12345u;
        r[i].w = 8 + (int)((s >> 16) % 56);
        s = s * 1103515245u + 12345u;
        r[i].h = 8 + (int)((s >> 16) % 56);
    }
    AvolLimits lim = {256, 256, 256 * 256 * 3 / 4, 3, scale};
    AvolBook b;
    AvolStats st;
    int err = -1;
    double t0 = now_ms();
    int rc = avol_pack(r, N, &lim, &b, &st, &err, 1);
    double dt = now_ms() - t0;
    if (rc != AVOL_OK) {
        printf("stress scale=%d failed rc=%d err_item=%d\n", scale, rc, err);
        return;
    }
    int bad = -1;
    long parsed = 0;
    int ok = avol_validate(&b, r, &bad) == AVOL_OK &&
             avol_probe(&b, r, &parsed, &bad) == AVOL_OK;
    printf("\n== 压力算例：64 块，每册 256x256，cap=%ld 字节，scale=%dx，"
           "pack %.3f ms ==\n", lim.cap_bytes, scale, dt);
    printf("  volumes=%d index_len=%d index_bytes=%d samples=%ld "
           "used=%ld waste=%ld occupancy=%.4f\n",
           st.volumes, st.index_len, st.index_bytes, st.samples,
           st.used_px, st.waste_px, st.occupancy);
    printf("  validate+probe: %s (parsed=%ld)\n", ok ? "OK" : "FAIL", parsed);
    for (int p = 0; p < b.volume_count; ++p)
        printf("  vol %d: rects=%d used_px=%d shelves=%d occupancy=%.4f "
               "bytes=%ld/%ld\n",
               p, b.vols[p].rects, b.vols[p].used_px, b.vols[p].shelves,
               b.vols[p].occupancy, b.vols[p].used_bytes, lim.cap_bytes);
    printf("  samples==parsed: %s\n", st.samples == parsed ? "OK" : "MISMATCH");
    avol_free(&b);
}

/* 老路径：关掉新逻辑后与 atlas_pack 逐项相同。 */
static void case_legacy(void) {
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos pos[4];
    AtlasStats old;
    if (atlas_pack(r, 4, 64, pos, &old) != 0) return;
    AvolLimits lim = {64, 64, 64 * 64 * 3, 3, 1};
    AvolBook b;
    AvolStats st;
    int err = -1;
    if (avol_pack(r, 4, &lim, &b, &st, &err, 0) != AVOL_OK) return;
    int same = b.volume_count == 1 && b.vols[0].used_px == old.used_px;
    for (int i = 0; i < 4; ++i)
        if (b.cells[i].volume != 0 || b.cells[i].x != pos[i].x ||
            b.cells[i].y != pos[i].y) same = 0;
    printf("\n== 老路径（enable_new=0）：与 atlas_pack 逐项相同: %s ==\n",
           same ? "OK" : "MISMATCH");
    printf("  shelves=%d used_px=%d index_len=%d\n",
           b.vols[0].shelves, b.vols[0].used_px, st.index_len);
    avol_free(&b);
}

int main(void) {
    printf("atlas volume split (ATLAS_USE_VOL=1)\n");
    case_hand(1);
    case_hand(2);
    case_legacy();
    case_stress(1);
    case_stress(2);
    return 0;
}

#endif /* ATLAS_USE_VOL */
