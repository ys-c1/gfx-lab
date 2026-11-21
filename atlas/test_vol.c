#include "atlas_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !ATLAS_USE_VOL

int main(void) {
    printf("atlas_vol: disabled (build with -DATLAS_USE_VOL=1)\n");
    return 0;
}

#else

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 8 个 32x16：每册 64x32、每像素 3 字节。1x 下每册恰好 4 块，2x 下每册 1 块。 */
static AtlasRect g_r[8];
static void make_rects(void) {
    for (int i = 0; i < 8; ++i) { g_r[i].w = 32; g_r[i].h = 16; }
}

static AvolLimits lim(int scale) {
    AvolLimits l = {64, 32, 6144, 3, scale};
    return l;
}

/* 读回整个临时文件用于逐字节比较导出文本。 */
static long slurp(FILE *f, char *buf, long cap) {
    fflush(f);
    fseek(f, 0, SEEK_SET);
    long n = (long)fread(buf, 1, (size_t)cap, f);
    return n;
}

int main(void) {
    make_rects();

    /* ===== 阶段一：切册，只写 vol_of ===== */
    {
        int vol_of[8], nvol = 0, err = -1;
        AvolLimits l1 = lim(1);
        CK(avol_split(g_r, 8, &l1, vol_of, &nvol, &err) == AVOL_OK);
        CK(nvol == 2 && err == -1);
        CK(vol_of[0] == 0 && vol_of[3] == 0 && vol_of[4] == 1 && vol_of[7] == 1);

        AvolLimits l2 = lim(2);   /* 同一 cap_bytes：每册只装得下 1 块 */
        CK(avol_split(g_r, 8, &l2, vol_of, &nvol, &err) == AVOL_OK);
        CK(nvol == 8);
        int one_each = 1;
        for (int i = 0; i < 8; ++i) if (vol_of[i] != i) one_each = 0;
        CK(one_each);
    }

    /* ===== 阶段二：1x 回填，与纸笔对照 ===== */
    {
        AvolLimits l1 = lim(1);
        int vol_of[8], nvol = 0, err = -1;
        CK(avol_split(g_r, 8, &l1, vol_of, &nvol, &err) == AVOL_OK);
        AvolBook b;
        AvolStats st;
        CK(avol_layout(g_r, 8, vol_of, nvol, &l1, &b, &st, &err) == AVOL_OK);

        /* 册内货架：两块并排、再起一层 */
        CK(b.cells[0].volume == 0 && b.cells[0].x == 0 && b.cells[0].y == 0);
        CK(b.cells[1].volume == 0 && b.cells[1].x == 32 && b.cells[1].y == 0);
        CK(b.cells[2].volume == 0 && b.cells[2].x == 0 && b.cells[2].y == 16);
        CK(b.cells[3].volume == 0 && b.cells[3].x == 32 && b.cells[3].y == 16);
        CK(b.cells[4].volume == 1 && b.cells[4].x == 0 && b.cells[4].y == 0);

        /* 纸笔：每册 4*512=2048 点 = 64*32，占满；2 册合计 4096 点、12288 字节 */
        CK(st.volumes == 2);
        CK(st.used_px == 4096 && st.used_bytes == 12288);
        CK(st.capacity_px == 4096 && st.waste_px == 0 && st.waste_bytes == 0);
        CK(st.occupancy > 0.9999 && st.occupancy <= 1.0);
        CK(st.samples == 4096);
        /* 索引：16 + 12*8 + 20*2 = 152 字节，10 条记录 */
        CK(st.index_len == 10 && st.index_bytes == 152);
        for (int p = 0; p < 2; ++p) {
            CK(b.vols[p].rects == 4);
            CK(b.vols[p].used_px == 2048 && b.vols[p].used_bytes == 6144);
            CK(b.vols[p].shelves == 2);
            CK(b.vols[p].occupancy > 0.9999);
        }

        /* 校验、逐像素解析、越界检出 */
        int bad = -99;
        CK(avol_validate(&b, g_r, &bad) == AVOL_OK && bad == -1);
        long samples = 0;
        CK(avol_probe(&b, g_r, &samples, &bad) == AVOL_OK);
        CK(samples == st.samples);

        int vol; long off;
        CK(avol_texel(&b, g_r, 1, 0, 0, &vol, &off, &bad) == AVOL_OK);
        CK(vol == 0 && off == (1L * 32 + 0) * 3);          /* 第 1 行第 32 列 */
        CK(avol_texel(&b, g_r, 1, 31, 15, &vol, &off, &bad) == AVOL_OK);
        CK(vol == 0 && off == (15L * 64 + 63) * 3);
        CK(avol_texel(&b, g_r, 1, 32, 0, &vol, &off, &bad) == AVOL_ERR_OOB);
        CK(bad == 1);                                       /* 越界报矩形序号 */
        CK(avol_texel(&b, g_r, 1, 0, 16, &vol, &off, &bad) == AVOL_ERR_OOB);
        CK(bad == 1);
        AvolCell cell;
        CK(avol_lookup(&b, 8, &cell, &bad) == AVOL_ERR_OOB && bad == 8);

        /* 人为破坏索引：越界检出并指出矩形 */
        b.cells[5].x = 64;
        CK(avol_validate(&b, g_r, &bad) == AVOL_ERR_OOB && bad == 5);
        b.cells[5].x = 0;

        avol_free(&b);
    }

    /* ===== 2x：册数、占用、采样次数按倍率放大 ===== */
    {
        AvolLimits l2 = lim(2);
        AvolBook b;
        AvolStats st;
        int err = -1;
        CK(avol_pack(g_r, 8, &l2, &b, &st, &err, 1) == AVOL_OK);
        /* 纸笔：每册 1 块 512 点 * 4 = 2048 物理像素 * 3 = 6144 字节 */
        CK(st.volumes == 8);
        CK(st.used_px == 4096 && st.used_bytes == 49152);
        CK(st.capacity_px == 16384 && st.waste_px == 12288);
        CK(st.waste_bytes == 147456);
        CK(st.occupancy > 0.2499 && st.occupancy < 0.2501);
        CK(st.samples == 16384);
        CK(st.index_len == 16 && st.index_bytes == 272);
        for (int p = 0; p < 8; ++p) {
            CK(b.vols[p].rects == 1);
            CK(b.vols[p].used_px == 512 && b.vols[p].used_bytes == 6144);
            CK(b.vols[p].used_bytes <= l2.cap_bytes);
        }
        int bad = -99;
        CK(avol_validate(&b, g_r, &bad) == AVOL_OK);
        long samples = 0;
        CK(avol_probe(&b, g_r, &samples, &bad) == AVOL_OK && samples == st.samples);
        avol_free(&b);
    }

    /* ===== 越界输入 ===== */
    {
        AtlasRect big[2] = {{8, 8}, {128, 16}};
        AvolLimits l1 = lim(1);
        AvolBook b;
        AvolStats st;
        int err = -99;
        CK(avol_pack(big, 2, &l1, &b, &st, &err, 1) == AVOL_ERR_TOO_BIG && err == 1);
        CK(b.cells == NULL);            /* 失败后 book 已清零，free 安全 */
        avol_free(&b);
        CK(avol_pack(g_r, 0, &l1, &b, &st, &err, 1) == AVOL_ERR_ARG);
        CK(avol_pack(NULL, 8, &l1, &b, &st, &err, 1) == AVOL_ERR_ARG);
        AvolLimits bad_scale = lim(3);
        CK(avol_pack(g_r, 8, &bad_scale, &b, &st, &err, 1) == AVOL_ERR_ARG);
    }

    /* ===== 关掉新逻辑：仍走 atlas_pack，导出字节不变 ===== */
    {
        AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
        AtlasPos old_pos[4];
        AtlasStats old_stats;
        CK(atlas_pack(r, 4, 64, old_pos, &old_stats) == 0);

        AvolLimits l1 = {64, 64, 64 * 64 * 3, 3, 1};
        AvolBook legacy, fresh;
        AvolStats ls, fs;
        int err = -1;
        CK(avol_pack(r, 4, &l1, &legacy, &ls, &err, 0) == AVOL_OK);
        CK(avol_pack(r, 4, &l1, &fresh, &fs, &err, 1) == AVOL_OK);

        /* 册内坐标与 atlas_pack 逐项相同 */
        int same = 1;
        for (int i = 0; i < 4; ++i)
            if (legacy.cells[i].volume != 0 ||
                legacy.cells[i].x != old_pos[i].x ||
                legacy.cells[i].y != old_pos[i].y ||
                legacy.cells[i].placed != old_pos[i].placed) same = 0;
        CK(same);
        CK(legacy.vols[0].used_px == old_stats.used_px);
        CK(legacy.vols[0].shelves == old_stats.shelves);
        CK(fresh.volume_count == 1);
        CK(memcmp(legacy.cells, fresh.cells, sizeof(AvolCell) * 4) == 0);

        /* 导出文本逐字节相同 */
        FILE *fa = tmpfile(), *fb = tmpfile();
        CK(fa && fb);
        CK(avol_export(&legacy, r, fa) == AVOL_OK);
        CK(avol_export(&fresh, r, fb) == AVOL_OK);
        static char ba[8192], bb[8192];
        long na = slurp(fa, ba, sizeof ba), nb = slurp(fb, bb, sizeof bb);
        CK(na > 0 && na == nb && memcmp(ba, bb, (size_t)na) == 0);
        fclose(fa);
        fclose(fb);

        CK(ls.index_len == fs.index_len && ls.index_bytes == fs.index_bytes);
        CK(ls.samples == fs.samples && ls.used_bytes == fs.used_bytes);
        avol_free(&legacy);
        avol_free(&fresh);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}

#endif /* ATLAS_USE_VOL */
