#include "cull.h"
#include "cull_inc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 全量参照：直接调用 cull_run，返回按 id 升序的可见集合（cull_run 本身即升序） */
static int full_visible(const CullBox *b, int n, CullView v, int **ids, int *cnt) {
    CullResult r;
    int rc = cull_run(b, n, v, &r);
    if (rc != 0) return rc;
    *ids = r.ids; *cnt = r.count;
    return 0;
}

/* 逐项比较增量结果与全量结果，返回不一致条数（误差计数） */
static int compare_sets(const int *inc, int inc_n, const int *full, int full_n) {
    int err = 0;
    if (inc_n != full_n) {
        printf("  count mismatch inc=%d full=%d\n", inc_n, full_n);
        ++err;
    }
    int m = inc_n < full_n ? inc_n : full_n;
    for (int i = 0; i < m; ++i)
        if (inc[i] != full[i]) { printf("  idx %d: inc=%d full=%d\n", i, inc[i], full[i]); ++err; }
    return err;
}

static CullBox *make_grid(int side, double spacing, double size) {
    int n = side * side;
    CullBox *b = malloc(sizeof(CullBox) * (size_t)n);
    for (int i = 0; i < n; ++i) {
        b[i].x = (i % side) * spacing;
        b[i].y = (i / side) * spacing;
        b[i].w = size; b[i].h = size;
        b[i].layer = i % 3;
    }
    return b;
}

int main(void) {
    /* ---- 1. 与现有 cull_run 一致性（小例子，多帧平移/缩放） ---- */
    {
        enum { S = 20, N = S * S };
        CullBox *b = make_grid(S, 20.0, 18.0);
        CullView start = {0, 0, 120, 120};
        CullInc *st = NULL; const char *err = NULL;
        CK(cull_inc_open(b, N, start, &st, &err) == CULL_OK);

        CullView moves[] = {
            {2, 2, 120, 120},      /* 微小平移：只动一点点 */
            {5, 0, 120, 120},
            {40, 40, 120, 120},    /* 较大平移 */
            {40, 40, 200, 90},     /* 缩放：宽高都变 */
            {-10, 5, 80, 300},     /* 越过原点、非等比 */
            {0, 0, 120, 120},      /* 回到起点 */
        };
        CullView prev = start;
        for (size_t f = 0; f < sizeof(moves)/sizeof(moves[0]); ++f) {
            CullIncStats s;
            int rc = cull_inc_update(st, moves[f], &s, &err);
            CK(rc == CULL_OK);
            CullResult snap;
            CK(cull_inc_snapshot(st, &snap) == CULL_OK);
            int *fid = NULL; int fn = 0;
            CK(full_visible(b, N, moves[f], &fid, &fn) == 0);
            int e = compare_sets(snap.ids, snap.count, fid, fn);
            CK(e == 0);
            /* 更新数与实际进出一致 */
            CK(s.updated == s.entered + s.exited);
            CK(s.candidates >= s.updated);
            if (moves[f].x == prev.x && moves[f].y == prev.y &&
                moves[f].w == prev.w && moves[f].h == prev.h) {
                CK(s.candidates == 0 && s.updated == 0);
            } else {
                CK(s.updated >= 0 && s.candidates >= 0);
            }
            free(snap.ids); free(fid);
            prev = moves[f];
        }
        cull_inc_close(st);
        free(b);
    }

    /* ---- 2. 无差异：集合完全一样、零更新零候选 ---- */
    {
        enum { N = 4 };
        CullBox b[N] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
        CullView v = {0, 0, 60, 60};
        CullInc *st = NULL;
        CK(cull_inc_open(b, N, v, &st, NULL) == CULL_OK);
        CullResult before; CK(cull_inc_snapshot(st, &before) == 0);
        CullIncStats s;
        CK(cull_inc_update(st, v, &s, NULL) == CULL_OK);   /* 同一视口 */
        CullResult after; CK(cull_inc_snapshot(st, &after) == 0);
        CK(before.count == after.count);
        CK(memcmp(before.ids, after.ids, sizeof(int)*(size_t)before.count) == 0);
        CK(s.candidates == 0 && s.entered == 0 && s.exited == 0 && s.updated == 0);
        free(before.ids); free(after.ids);
        cull_inc_close(st);
    }

    /* ---- 3. 差异为负 / 空视口报错，并指出字段 ---- */
    {
        CullBox b[1] = {{0,0,10,10,0}};
        CullInc *st = NULL; const char *err = NULL;
        CullView good = {0,0,50,50};
        CK(cull_inc_open(b, 1, good, &st, &err) == CULL_OK);

        CullView negw = {0,0,-5,50};
        err = NULL;
        CK(cull_inc_update(st, negw, NULL, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "new_view.w") && strstr(err, "negative"));
        printf("  err: %s\n", err);

        CullView negh = {0,0,50,-1};
        err = NULL;
        CK(cull_inc_update(st, negh, NULL, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "new_view.h") && strstr(err, "negative"));
        printf("  err: %s\n", err);

        CullView empty = {0,0,0,50};
        err = NULL;
        CK(cull_inc_update(st, empty, NULL, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "empty"));
        printf("  err: %s\n", err);

        /* open 阶段的空视口与负尺寸 */
        CullInc *st2 = NULL; err = NULL;
        CK(cull_inc_open(b, 1, negw, &st2, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "view.w"));
        printf("  err: %s\n", err);

        /* cull_diff 单独校验，指出 old/new 角色与字段 */
        CullDiff d; err = NULL;
        CK(cull_diff(good, negh, &d, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "new.h"));
        printf("  err: %s\n", err);
        err = NULL;
        CK(cull_diff(negw, good, &d, &err) == CULL_E_VIEW_EMPTY);
        CK(err && strstr(err, "old.w"));
        printf("  err: %s\n", err);

        /* 负尺寸盒子 */
        CullBox badbox[2] = {{0,0,10,10,0},{5,5,-2,10,0}};
        err = NULL;
        CK(cull_inc_open(badbox, 2, good, &st2, &err) == CULL_E_BOX);
        CK(err && strstr(err, "boxes[1].w"));
        printf("  err: %s\n", err);

        cull_inc_close(st);
    }

    /* ---- 4. cull_diff 几何：平移时 enter/exit 带状区域 ---- */
    {
        CullView o = {0, 0, 100, 100};
        CullView n = {10, 0, 100, 100};          /* 右移 10 */
        CullDiff d;
        CK(cull_diff(o, n, &d, NULL) == CULL_OK);
        CK(d.moved);
        /* enter: 新视口右边进入的竖条 x∈[100,110] */
        CK(d.enter.x == 100 && d.enter.w == 10 && d.enter.h == 100);
        /* exit: 旧视口左边退出的竖条 x∈[0,10] */
        CK(d.exit_.x == 0 && d.exit_.w == 10 && d.exit_.h == 100);

        CullView same = o;
        CK(cull_diff(o, same, &d, NULL) == CULL_OK);
        CK(!d.moved && d.enter.w == 0 && d.exit_.w == 0);

        /* 放大：只有 enter，没有 exit */
        CullView big = {-10, -10, 120, 120};
        CK(cull_diff(o, big, &d, NULL) == CULL_OK);
        CK(d.enter.w > 0 && d.exit_.w == 0 && d.exit_.h == 0);
    }

    /* ---- 5. 随机压力：长序列随机视口，每帧逐项对齐全量 ---- */
    {
        enum { S = 40, N = S * S, FRAMES = 300 };
        CullBox *b = make_grid(S, 10.0, 8.0);
        srand(12345);
        CullView v = {0, 0, 50, 50};
        CullInc *st = NULL;
        CK(cull_inc_open(b, N, v, &st, NULL) == CULL_OK);
        long total_upd = 0, total_cand = 0;
        int total_err = 0;
        for (int f = 0; f < FRAMES; ++f) {
            CullView nv;
            nv.x = (rand() % 200) - 50;
            nv.y = (rand() % 200) - 50;
            nv.w = 20 + rand() % 150;
            nv.h = 20 + rand() % 150;
            CullIncStats s;
            CK(cull_inc_update(st, nv, &s, NULL) == CULL_OK);
            CullResult snap; CK(cull_inc_snapshot(st, &snap) == 0);
            int *fid = NULL, fn = 0;
            CK(full_visible(b, N, nv, &fid, &fn) == 0);
            total_err += compare_sets(snap.ids, snap.count, fid, fn);
            total_upd += s.updated; total_cand += s.candidates;
            free(snap.ids); free(fid);
            v = nv;
        }
        CK(total_err == 0);
        printf("  stress: %d frames, total updates=%ld candidates=%ld (N=%d)\n",
               FRAMES, total_upd, total_cand, N);
        CK(total_cand < (long)N * FRAMES);   /* 增量确实没在全量扫描 */
        cull_inc_close(st);
        free(b);
    }

    /* ---- 6. 空集合 n=0 不崩溃 ---- */
    {
        CullInc *st = NULL; CullView v = {0,0,10,10};
        CK(cull_inc_open(NULL, 0, v, &st, NULL) == CULL_E_ARG);
        CK(cull_inc_open((CullBox*)0, 0, v, &st, NULL) == CULL_E_ARG);
        CullBox dummy[1] = {{0,0,1,1,0}};
        CK(cull_inc_open(dummy, 0, v, &st, NULL) == CULL_OK);
        CullIncStats s; CullView nv = {2,2,10,10};
        CK(cull_inc_update(st, nv, &s, NULL) == CULL_OK);
        CK(s.updated == 0);
        cull_inc_close(st);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
