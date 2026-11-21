#include "atlas.h"
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 存活块两两不重叠 */
static int no_overlap(const Atlas *a) {
    for (int i = 0; i < a->n; ++i) {
        if (!a->blocks[i].live) return 0; /* 索引必须连续压缩到 0..n-1 */
        for (int j = i + 1; j < a->n; ++j) {
            const AtlasBlock *p = &a->blocks[i], *q = &a->blocks[j];
            if (p->pos.x < q->pos.x + q->rect.w && q->pos.x < p->pos.x + p->rect.w &&
                p->pos.y < q->pos.y + q->rect.h && q->pos.y < p->pos.y + p->rect.h)
                return 0;
        }
    }
    return 1;
}

/* 固定场景：确定性矩形流 + 触摸，驱动回收 */
static void run_scenario(Atlas *a) {
    static const AtlasRect rr[10] = {{12,8},{20,12},{8,16},{16,8},{24,8},
                                     {10,10},{14,6},{18,14},{6,20},{22,10}};
    unsigned st = 7;
    for (int i = 0; i < 60; ++i) {
        if (a->n > 0) { /* 确定性触摸，制造 LRU 差异 */
            st = st * 1103515245u + 12345u;
            atlas_touch(a, (int)((st >> 16) % (unsigned)a->n));
        }
        atlas_add(a, rr[i % 10], NULL);
    }
}

int main(void) {
    /* ---- 旧路径原有检查（保持不变） ---- */
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);

    /* ---- 负参数报错并指出是哪一项 ---- */
    AtlasRect rn[5] = {{8,8},{8,8},{-4,8},{8,8},{8,8}};
    AtlasPos pn[5];
    AtlasStats sn;
    int item = -1;
    CK(atlas_pack_ex(rn, 5, 64, pn, &sn, &item) == ATLAS_ERECT && item == 2);
    Atlas a;
    CK(atlas_open(&a, -64, 64, 16, 1) == ATLAS_EARG);
    CK(atlas_open(&a, 64, -64, 16, 1) == ATLAS_EARG);
    CK(atlas_open(&a, 64, 64, -16, 1) == ATLAS_EARG);
    CK(atlas_open(&a, 64, 64, 16, 0) == ATLAS_OK);
    item = -1;
    CK(atlas_add(&a, (AtlasRect){-8, 8}, &item) == ATLAS_ERECT && item == 0);
    AtlasRect rm[6] = {{8,8},{8,8},{8,8},{8,8},{8,-2},{8,8}};
    item = -1;
    CK(atlas_add_many(&a, rm, 6, &item) == ATLAS_ERECT && item == 4);
    atlas_close(&a);

    /* ---- 关掉回收：输出必须回到旧路径 ---- */
    CK(atlas_open(&a, 64, 4096, 16, 0) == ATLAS_OK);
    for (int i = 0; i < 4; ++i) {
        int slot = atlas_add(&a, r[i], NULL);
        CK(slot == i);
        CK(a.blocks[slot].pos.x == p[i].x && a.blocks[slot].pos.y == p[i].y);
    }
    CK(a.used_px == s.used_px);
    CK(a.shelves == s.shelves);
    atlas_close(&a);

    /* ---- 关掉回收且图集满：报 ATLAS_EFULL，不崩溃 ---- */
    CK(atlas_open(&a, 32, 32, 8, 0) == ATLAS_OK);
    CK(atlas_add(&a, (AtlasRect){32, 32}, NULL) == 0);
    CK(atlas_add(&a, (AtlasRect){16, 16}, NULL) == ATLAS_EFULL);
    atlas_close(&a);

    /* ---- LRU：先标记最近使用，回收从最久未用开始 ---- */
    CK(atlas_open(&a, 32, 16, 8, 1) == ATLAS_OK); /* 恰好放 2 块 16x16 */
    CK(atlas_add(&a, (AtlasRect){16, 16}, NULL) == 0); /* id 0 (A) */
    CK(atlas_add(&a, (AtlasRect){16, 16}, NULL) == 1); /* id 1 (B) */
    CK(atlas_touch(&a, 0) == ATLAS_OK);                /* A 最近被用 */
    CK(atlas_add(&a, (AtlasRect){16, 16}, NULL) >= 0); /* 触发回收 */
    long cnt = 0;
    const int *log = atlas_recycled(&a, &cnt);
    CK(cnt == 1 && log && log[0] == 1); /* 最久未用的 B(id 1) 被回收 */
    CK(a.n == 2 && a.blocks[0].id == 0 && a.blocks[1].id == 2); /* 索引已重排 */
    atlas_close(&a);

    /* ---- 回收顺序可复现 + 占用率 + 重排后无重叠 ---- */
    Atlas b1, b2;
    CK(atlas_open(&b1, 64, 48, 64, 1) == ATLAS_OK);
    CK(atlas_open(&b2, 64, 48, 64, 1) == ATLAS_OK);
    run_scenario(&b1);
    run_scenario(&b2);
    long c1 = 0, c2 = 0;
    const int *l1 = atlas_recycled(&b1, &c1);
    const int *l2 = atlas_recycled(&b2, &c2);
    CK(c1 > 0);                 /* 确实发生了回收 */
    CK(c1 == c2);               /* 回收块数一致 */
    int same = 1;
    for (long i = 0; i < c1; ++i) if (l1[i] != l2[i]) same = 0;
    CK(same);                   /* 回收顺序可复现 */
    CK(no_overlap(&b1));        /* 重排索引后无重叠、槽位连续 */
    double occ = atlas_occupancy(&b1);
    CK(occ > 0.0 && occ <= 1.0); /* 占用率合法 */
    atlas_close(&b1);
    atlas_close(&b2);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
