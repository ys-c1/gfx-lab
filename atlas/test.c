#include "atlas.h"
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static AtlasRect SRC0[3] = {{16,16},{32,8},{8,8}};
static AtlasRect SRC1[3] = {{64,4},{24,12},{12,24}};
static AtlasRect SRC2[4] = {{40,10},{16,16},{10,40},{48,6}};

static void build_sources(AtlasSource *s) {
    s[0] = (AtlasSource){SRC0, 3, -1};
    s[1] = (AtlasSource){SRC1, 3, -1};
    s[2] = (AtlasSource){SRC2, 4, -1};
}

/* 手算期望值：合并模式 width=128 的货架布局（按输入顺序、遇宽换行）。
 * 货架1 y=0:  (16x16)x0  (32x8)x16  (8x8)x48  (64x4)x56   末cur_x=120 高16
 * 货架2 y=16: (24x12)x0 (12x24)x24 (40x10)x36 (16x16)x76 (10x40)x92 末102 高40
 * 货架3 y=56: (48x6)x0                                          高6
 * 占用带高 = 16+40+6 = 62；used = 256+256+64+256+288+288+400+256+400+288 = 2752
 */
static void test_merge_128(void) {
    AtlasSource s[3]; build_sources(s);
    AtlasPos pos[10];
    AtlasMerge m = {128, 1, s, 3, pos, {0}};
    int err_at = -1;
    CK(atlas_merge_layout(&m, &err_at) == 0);
    CK(s[0].base == 0 && s[1].base == 3 && s[2].base == 6);
    CK(m.stats.total_blocks == 10);
    CK(m.stats.used_px == 2752);
    CK(m.stats.shelves == 3);
    CK(m.stats.total_height == 62);
    /* 逐块坐标手算对照。 */
    CK(pos[0].x == 0  && pos[0].y == 0);
    CK(pos[1].x == 16 && pos[1].y == 0);
    CK(pos[2].x == 48 && pos[2].y == 0);
    CK(pos[3].x == 56 && pos[3].y == 0);
    CK(pos[4].x == 0  && pos[4].y == 16);
    CK(pos[5].x == 24 && pos[5].y == 16);
    CK(pos[6].x == 36 && pos[6].y == 16);
    CK(pos[7].x == 76 && pos[7].y == 16);
    CK(pos[8].x == 92 && pos[8].y == 16);
    CK(pos[9].x == 0  && pos[9].y == 56);

    /* 改写：new_idx = base + old_idx，全部有效。 */
    AtlasIndexEntry ent[10]; int k = 0;
    for (int si = 0; si < 3; ++si)
        for (int i = 0; i < s[si].nblocks; ++i)
            ent[k++] = (AtlasIndexEntry){si, i, -1, 0};
    int err_slot = -1;
    CK(atlas_rewrite_indices(&m, ent, 10, &err_at, &err_slot) == 0);
    CK(m.stats.rewritten == 10 && m.stats.errors == 0);
    CK(ent[0].new_idx == 0 && ent[3].new_idx == 3 && ent[6].new_idx == 6);
    CK(ent[9].new_idx == 9);
    for (int i = 0; i < 10; ++i) CK(ent[i].valid == 1);
}

/* 合并模式 width=512：全部块放进一个货架，占用带高=最高块 40。 */
static void test_merge_512(void) {
    AtlasSource s[3]; build_sources(s);
    AtlasPos pos[10];
    AtlasMerge m = {512, 1, s, 3, pos, {0}};
    int err_at = -1;
    CK(atlas_merge_layout(&m, &err_at) == 0);
    CK(m.stats.shelves == 1);
    CK(m.stats.total_height == 40);
    CK(m.stats.used_px == 2752);
    CK(pos[9].x == 16+32+8+64+24+12+40+16+10); /* 222 */
    CK(pos[9].y == 0);
}

/* 不合并：每个源独立货架带，base 仍连续；src0 内部恒等映射。 */
static void test_flat_128(void) {
    AtlasSource s[3]; build_sources(s);
    AtlasPos pos[10];
    AtlasMerge m = {128, 0, s, 3, pos, {0}};
    int err_at = -1;
    CK(atlas_merge_layout(&m, &err_at) == 0);
    CK(s[0].base == 0 && s[1].base == 3 && s[2].base == 6);
    /* src0：3 块均在 x 方向排开，第一货架 y=0。 */
    CK(pos[0].x == 0 && pos[1].x == 16 && pos[2].x == 48);
    CK(pos[0].y == 0 && pos[1].y == 0 && pos[2].y == 0);
    /* src1 从新的货架带开始（y>0），不与 src0 重叠。 */
    CK(pos[3].x == 0 && pos[3].y > 0);
    /* 恒等改写：每条 new_idx 等于其全局序号。 */
    AtlasIndexEntry ent[10]; int k = 0;
    for (int si = 0; si < 3; ++si)
        for (int i = 0; i < s[si].nblocks; ++i)
            ent[k++] = (AtlasIndexEntry){si, i, -1, 0};
    int err_slot = -1;
    CK(atlas_rewrite_indices(&m, ent, 10, &err_at, &err_slot) == 0);
    for (int i = 0; i < 10; ++i) CK(ent[i].new_idx == i && ent[i].valid == 1);
}

static void test_errors(void) {
    AtlasSource s[3]; build_sources(s);
    AtlasPos pos[10];
    AtlasMerge m = {128, 1, s, 3, pos, {0}};
    int err_at = -1, err_slot = -1;
    CK(atlas_merge_layout(&m, &err_at) == 0);

    /* 偏移越界：old_idx 超出该源块数，首个出错条目序号=1。 */
    AtlasIndexEntry bad[3] = {{0,0,-1,0},{0,99,-1,0},{7,0,-1,0}};
    CK(atlas_rewrite_indices(&m, bad, 3, &err_at, &err_slot) == -4);
    CK(err_at == 1);
    CK(m.stats.errors == 2 && m.stats.rewritten == 1);

    /* 索引冲突：两条引用落到同一全局槽位 0。 */
    AtlasIndexEntry dup[2] = {{0,0,-1,0},{0,0,-1,0}};
    CK(atlas_rewrite_indices(&m, dup, 2, &err_at, &err_slot) == -5);
    CK(err_at == 1 && err_slot == 0);
    CK(dup[0].valid == 1 && dup[1].valid == 0);

    /* 超宽块：阶段一返回 -2，给全局块序号。 */
    AtlasRect wide[1] = {{256,4}};
    AtlasSource ws[1] = {{wide,1,-1}};
    AtlasPos wp[1];
    AtlasMerge wm = {128,1,ws,1,wp,{0}};
    CK(atlas_merge_layout(&wm, &err_at) == -2);
    CK(err_at == 0);

    /* 非法参数。 */
    CK(atlas_merge_layout(0, &err_at) == -1);
}

static void test_pack_legacy(void) {
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4]; AtlasStats st;
    CK(atlas_pack(r, 4, 64, p, &st) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(st.shelves >= 2);
    CK(st.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &st) == -2);
    CK(atlas_pack(r, 0, 64, p, &st) == -1);
}

static void test_export(void) {
    AtlasSource s[3]; build_sources(s);
    AtlasPos pos[10];
    AtlasMerge m = {128, 1, s, 3, pos, {0}};
    int err_at = -1, err_slot = -1;
    atlas_merge_layout(&m, &err_at);
    AtlasIndexEntry ent[10]; int k = 0;
    for (int si = 0; si < 3; ++si)
        for (int i = 0; i < s[si].nblocks; ++i)
            ent[k++] = (AtlasIndexEntry){si, i, -1, 0};
    atlas_rewrite_indices(&m, ent, 10, &err_at, &err_slot);
    CK(atlas_export_offsets(&m, "build/t_offsets.txt") == 0);
    CK(atlas_export_indices(ent, 10, "build/t_indices.txt") == 0);
    CK(atlas_export_offsets(&m, "/nonexistent/dir/x.txt") == -6);
}

int main(void) {
    test_pack_legacy();
    test_merge_128();
    test_merge_512();
    test_flat_128();
    test_errors();
    test_export();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
