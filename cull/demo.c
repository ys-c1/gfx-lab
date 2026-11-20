#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* 手推场景：视口 (0,0,100,100)，7 个对象，方向均为轴对齐或 45 度，便于手算。 */
static CullMovingBox B[7] = {
    /*x,  y,   w, h, layer, speed, dx, dy */
    { -5, -5, 10,10, 0, 10,  1,  0}, /* 0 已在视口，右行 */
    {105, 20, 10,10, 0, 20, -1,  0}, /* 1 右侧外，左行刚好扫入 */
    {120, 20, 10,10, 1, 10, -1,  0}, /* 2 太远，扫不到 */
    { 20,115, 10,10, 1, 20,  0, -1}, /* 3 下方外，上行扫入 */
    {115,115, 10,10, 2, 40, -1, -1}, /* 4 对角高速，斜向扫入 */
    {150, 10, 10,10, 2, 40, -1,  0}, /* 5 太远，扫不到 */
    {105,105, 10,10, 0, 10, -1, -1}, /* 6 角点低速，斜向扫入 */
};
#define N 7

static int has_id_helper(const CullResult *r, int id) {
    for (int i = 0; i < r->count; ++i) if (r->ids[i] == id) return 1;
    return 0;
}

static void dump_scene(const char *path, CullView v) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "view %.6f %.6f %.6f %.6f\n", v.x, v.y, v.w, v.h);
    for (int i = 0; i < N; ++i)
        fprintf(f, "box %d %.6f %.6f %.6f %.6f %d %.6f %.6f %.6f\n",
                i, B[i].x, B[i].y, B[i].w, B[i].h, B[i].layer,
                B[i].speed, B[i].dx, B[i].dy);
    fclose(f);
    printf("scene written: %s\n", path);
}

static void dump_result(const char *path, double ratio,
                        const CullMotionReport *rep, const CullResult *r) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "ratio %.6f\n", ratio);
    for (int i = 0; i < N; ++i)
        fprintf(f, "expand %d %.6f %.6f\n", i, rep->ex[i], rep->ey[i]);
    fprintf(f, "kept %d", r->count);
    for (int i = 0; i < r->count; ++i) fprintf(f, " %d", r->ids[i]);
    fprintf(f, "\n");
    fclose(f);
    printf("result written: %s (ratio=%.2f)\n", path, ratio);
}

static void show_error(const char *tag, const CullMovingBox *boxes, int n,
                       double ratio, CullView v) {
    CullResult r; CullMotionReport rep; int err = -1;
    int rc = cull_motion(boxes, n, v, ratio, &r, &rep, &err);
    printf("[%s] rc=%d", tag, rc);
    if (err >= 0) printf(" -> 出错对象下标 idx=%d（第 %d 个）", err, err + 1);
    printf("\n");
    cull_report_free(&rep);  /* 报错路径可能已分配部分报告数组 */
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : "../artifacts";
    CullView v = {0, 0, 100, 100};

    printf("== 视口 (%.0f,%.0f,%.0f,%.0f)，扩展比例 ratio=1.0 ==\n", v.x, v.y, v.w, v.h);
    printf("扩展像素：ex=ratio*speed*|dx|/len，ey=ratio*speed*|dy|/len（单侧）\n\n");

    /* 阶段 1：扩盒 */
    CullBox expanded[N];
    CullMotionReport rep;
    int err = -1;
    if (cull_expand_boxes(B, N, 1.0, expanded, &rep, &err) != CULL_OK) {
        printf("expand error idx=%d\n", err);
        return 1;
    }

    /* 逐对象手推 */
    printf("%-3s %-22s %8s %8s | %-26s | %-6s %-6s %-6s\n",
           "idx", "起点盒(x,y,w,h)", "ex(px)", "ey(px)",
           "扩展后盒(x,y,w,h)", "朴素", "扫掠", "真值");
    CullResult naive, swept;
    CullBox plain[N];
    for (int i = 0; i < N; ++i) {
        plain[i].x = B[i].x; plain[i].y = B[i].y;
        plain[i].w = B[i].w; plain[i].h = B[i].h; plain[i].layer = B[i].layer;
    }
    cull_run(plain, N, v, &naive);
    /* 阶段 2：按扩展后盒剔除 */
    cull_run_swept(expanded, N, v, &swept);

    for (int i = 0; i < N; ++i) {
        int n_keep = has_id_helper(&naive, i);
        int s_keep = has_id_helper(&swept, i);
        /* 手推真值（ratio=1.0 时扫掠与真值一致）*/
        int truth[5] = {0, 1, 3, 4, 6};
        int t_keep = 0;
        for (int k = 0; k < 5; ++k) if (truth[k] == i) t_keep = 1;
        printf("%-3d (%5.0f,%5.0f,%4.0f,%4.0f) %8.3f %8.3f | (%6.2f,%6.2f,%6.2f,%6.2f) | %-6s %-6s %-6s\n",
               i, B[i].x, B[i].y, B[i].w, B[i].h, rep.ex[i], rep.ey[i],
               expanded[i].x, expanded[i].y, expanded[i].w, expanded[i].h,
               n_keep ? "保留" : "剔除", s_keep ? "保留" : "剔除",
               t_keep ? "可见" : "不可见");
    }

    int miss = 0, falsec = 0;
    for (int i = 0; i < N; ++i) {
        int truth[5] = {0, 1, 3, 4, 6};
        int t_keep = 0;
        for (int k = 0; k < 5; ++k) if (truth[k] == i) t_keep = 1;
        int s_keep = has_id_helper(&swept, i);
        if (t_keep && !s_keep) ++miss;   /* 漏剔：轨迹可见却被剔除 */
        if (!t_keep && s_keep) ++falsec; /* 误剔：扩过头多留 */
    }
    printf("\n朴素剔除保留 %d 个；扫掠剔除保留 %d 个；扫掠真值 5 个\n",
           naive.count, swept.count);
    printf("ratio=1.0：漏剔 %d，误剔 %d\n", miss, falsec);

    /* 比例可配：0.5 / 1.5 演示欠扩与过扩 */
    const double ratios[2] = {0.5, 1.5};
    for (int k = 0; k < 2; ++k) {
        CullResult rr; CullMotionReport rp; int e = -1;
        cull_motion(B, N, v, ratios[k], &rr, &rp, &e);
        int m = 0, fc = 0;
        for (int i = 0; i < N; ++i) {
            int truth[5] = {0, 1, 3, 4, 6};
            int t = 0;
            for (int q = 0; q < 5; ++q) if (truth[q] == i) t = 1;
            int keep = has_id_helper(&rr, i);
            if (t && !keep) ++m;
            if (!t && keep) ++fc;
        }
        printf("ratio=%.1f：保留 %d 个，漏剔 %d，误剔 %d\n",
               ratios[k], rr.count, m, fc);
        cull_report_free(&rp);
        free(rr.ids);
    }

    /* 报错演示：负速度、扩展为零（指出序号）*/
    CullMovingBox neg[2]  = {{0,0,10,10,0, 10,1,0}, {5,5,10,10,0, -3,1,0}};
    CullMovingBox zero1[2] = {{0,0,10,10,0, 5,1,0}, {9,9,10,10,0, 0,1,0}};
    CullMovingBox zero2[1] = {{9,9,10,10,0, 8,0,0}};
    show_error("负速度 ", neg, 2, 1.0, v);
    show_error("零速率 ", zero1, 2, 1.0, v);
    show_error("零方向 ", zero2, 1, 1.0, v);

    /* 不扩展时集合照旧 */
    CullResult r0; CullMotionReport rp0; int e0 = -1;
    cull_motion(B, N, v, 0.0, &r0, &rp0, &e0);
    printf("ratio=0（不扩展）保留 %d 个，与朴素剔除一致：%s\n",
           r0.count, r0.count == naive.count ? "是" : "否");
    free(r0.ids);

    /* 落盘供 Python 独立复核 */
    char path[512];
    snprintf(path, sizeof path, "%s/cull_scene.txt", dir);
    dump_scene(path, v);
    snprintf(path, sizeof path, "%s/cull_result_r1.0.txt", dir);
    dump_result(path, 1.0, &rep, &swept);

    cull_report_free(&rep);
    free(naive.ids);
    free(swept.ids);
    return 0;
}
