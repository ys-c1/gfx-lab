#include "atlas.h"
#include <stdio.h>
#include <time.h>

/* 三个源图集，共 10 个块，尺寸固定，便于逐块手算对照。 */
static AtlasRect SRC0[3] = {{16,16},{32,8},{8,8}};
static AtlasRect SRC1[3] = {{64,4},{24,12},{12,24}};
static AtlasRect SRC2[4] = {{40,10},{16,16},{10,40},{48,6}};
static AtlasSource SOURCES[3] = {
    {SRC0, 3, -1}, {SRC1, 3, -1}, {SRC2, 4, -1}
};
#define NSRC 3
#define TOTAL 10

static void run_width(int width, int do_merge, const char *tag) {
    AtlasSource src[NSRC];
    for (int i = 0; i < NSRC; ++i) src[i] = SOURCES[i];
    AtlasPos pos[TOTAL];
    AtlasMerge m = { width, do_merge, src, NSRC, pos, {0} };

    clock_t t0 = clock();
    int err_at = -1;
    int rc1 = atlas_merge_layout(&m, &err_at);
    clock_t t1 = clock();

    /* 旧索引：逐条引用每个块。 */
    AtlasIndexEntry ent[TOTAL];
    int k = 0;
    for (int s = 0; s < NSRC; ++s)
        for (int i = 0; i < src[s].nblocks; ++i)
            ent[k++] = (AtlasIndexEntry){ s, i, -1, 0 };

    int err_slot = -1;
    int rc2 = atlas_rewrite_indices(&m, ent, TOTAL, &err_at, &err_slot);
    clock_t t2 = clock();

    double ms_layout = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
    double ms_rewrite = (double)(t2 - t1) * 1000.0 / CLOCKS_PER_SEC;
    long canvas = (long)width * m.stats.total_height;
    double occ = canvas ? (double)m.stats.used_px / canvas : 0.0;

    printf("================ width=%d  %s  ================\n", width, tag);
    printf("layout_rc=%d rewrite_rc=%d\n", rc1, rc2);
    printf("占用率 used=%ldpx / canvas=%ldpx = %.4f%%  货架=%d 占用带高=%d\n",
           (long)m.stats.used_px, canvas, occ * 100.0, m.stats.shelves, m.stats.total_height);
    printf("改写条数 rewritten=%d / %d  错误 errors=%d  耗时 layout=%.4fms rewrite=%.4fms\n",
           m.stats.rewritten, TOTAL, m.stats.errors, ms_layout, ms_rewrite);

    printf("-- 偏移表（逐源）--\n");
    for (int s = 0; s < NSRC; ++s)
        printf("  src%d base=%d nblocks=%d\n", s, src[s].base, src[s].nblocks);

    printf("-- 逐块位置与改写对照（全局槽位 = base + old_idx）--\n");
    int off = 0;
    for (int s = 0; s < NSRC; ++s) {
        for (int i = 0; i < src[s].nblocks; ++i) {
            int g = off + i;
            AtlasRect r = src[s].blocks[i];
            printf("  entry%02d src%d old_idx=%d -> new_idx=%d valid=%d | "
                   "block %2dx%-2d @ (%3d,%3d)\n",
                   g, s, i, ent[g].new_idx, ent[g].valid, r.w, r.h,
                   pos[g].x, pos[g].y);
        }
        off += src[s].nblocks;
    }

    char opath[128], ipath[128];
    snprintf(opath, sizeof opath, "demo_offsets_w%d_%s.txt", width, do_merge ? "merge" : "flat");
    snprintf(ipath, sizeof ipath, "demo_indices_w%d_%s.txt", width, do_merge ? "merge" : "flat");
    int eo = atlas_export_offsets(&m, opath);
    int ei = atlas_export_indices(ent, TOTAL, ipath);
    printf("导出: %s rc=%d ; %s rc=%d\n\n", opath, eo, ipath, ei);
}

int main(void) {
    run_width(128, 1, "merge");
    run_width(512, 1, "merge");
    run_width(128, 0, "flat(不合并)");

    /* 越界与冲突注入：验证报错与序号。 */
    printf("================ 错误注入对照 ================\n");
    AtlasSource src[NSRC];
    for (int i = 0; i < NSRC; ++i) src[i] = SOURCES[i];
    AtlasPos pos[TOTAL];
    AtlasMerge m = { 128, 1, src, NSRC, pos, {0} };
    int err_at = -1, err_slot = -1;
    atlas_merge_layout(&m, &err_at);

    /* entry0 正常；entry1 old_idx 越界；entry2 src 越界。 */
    AtlasIndexEntry bad[3] = {
        {0, 0, -1, 0}, {0, 99, -1, 0}, {7, 0, -1, 0}
    };
    int rc = atlas_rewrite_indices(&m, bad, 3, &err_at, &err_slot);
    printf("越界注入 rc=%d (-4) 首个出错条目序号 err_at=%d (期望1)\n", rc, err_at);

    /* 冲突：两条引用都指向 src0 的 0 号块 -> 同一全局槽位。 */
    AtlasIndexEntry dup[2] = { {0, 0, -1, 0}, {0, 0, -1, 0} };
    rc = atlas_rewrite_indices(&m, dup, 2, &err_at, &err_slot);
    printf("冲突注入 rc=%d (-5) 冲突条目 err_at=%d (期望1) 槽位 err_slot=%d (期望0)\n",
           rc, err_at, err_slot);

    /* 超宽块：阶段一报错并给全局块序号。 */
    AtlasRect wide[1] = {{256, 4}};
    AtlasSource wsrc[1] = {{wide, 1, -1}};
    AtlasPos wpos[1];
    AtlasMerge wm = { 128, 1, wsrc, 1, wpos, {0} };
    rc = atlas_merge_layout(&wm, &err_at);
    printf("超宽块 rc=%d (-2) 出错全局块序号 err_at=%d (期望0)\n", rc, err_at);
    return 0;
}
