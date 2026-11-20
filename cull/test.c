#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static int has_id(const CullResult *r, int id) {
    for (int i = 0; i < r->count; ++i) if (r->ids[i] == id) return 1;
    return 0;
}
static int same_set(const CullResult *a, const CullResult *b) {
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; ++i) if (!has_id(b, a->ids[i])) return 0;
    return 1;
}

/* 旧接口测试：保持不动 */
static void test_legacy(void) {
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    free(r.ids);                       /* 释放成功结果，避免被下一次调用覆盖 */
    CullView bad = {0, 0, 0, 10};
    CullResult rbad = {NULL, 0};       /* 全新结果变量，验证错误路径不写入 */
    CK(cull_run(b, 4, bad, &rbad) == -2);
    CK(rbad.ids == NULL && rbad.count == 0);
}

/* 手推用例：视口 (0,0,100,100)，7 个对象（详见 demo 与回答中的逐对象推导） */
static CullMovingBox hand_boxes[7] = {
    /*x,  y,   w, h, layer, speed, dx, dy */
    { -5, -5, 10,10, 0, 10,  1,  0}, /* 0 已在视口，右行 */
    {105, 20, 10,10, 0, 20, -1,  0}, /* 1 右侧外，左行刚好扫入 */
    {120, 20, 10,10, 1, 10, -1,  0}, /* 2 太远，扫不到 */
    { 20,115, 10,10, 1, 20,  0, -1}, /* 3 下方外，上行扫入 */
    {115,115, 10,10, 2, 40, -1, -1}, /* 4 对角高速，斜向扫入 */
    {150, 10, 10,10, 2, 40, -1,  0}, /* 5 太远，扫不到 */
    {105,105, 10,10, 0, 10, -1, -1}, /* 6 角点低速，斜向扫入 */
};

static void test_motion(void) {
    CullView v = {0, 0, 100, 100};
    CullResult r;
    CullMotionReport rep;
    int err = -99;

    /* 比例 1.0：保留集合应为扫掠真值 {0,1,3,4,6}，漏剔 0、误剔 0 */
    CK(cull_motion(hand_boxes, 7, v, 1.0, &r, &rep, &err) == CULL_OK);
    CK(err == -1);
    CK(r.count == 5);
    static const int truth[5] = {0, 1, 3, 4, 6};
    for (int k = 0; k < 5; ++k) CK(has_id(&r, truth[k]));
    CK(!has_id(&r, 2) && !has_id(&r, 5));

    /* 扩展像素数核对（单侧） */
    CK(fabs(rep.ex[0] - 10.0) < 1e-12 && rep.ey[0] == 0.0);
    CK(fabs(rep.ex[1] - 20.0) < 1e-12);
    CK(rep.ex[3] == 0.0 && fabs(rep.ey[3] - 20.0) < 1e-12);
    CK(fabs(rep.ex[4] - 40.0 / sqrt(2.0)) < 1e-12);
    CK(fabs(rep.ey[4] - 40.0 / sqrt(2.0)) < 1e-12);
    CK(fabs(rep.ex[6] - 10.0 / sqrt(2.0)) < 1e-12);
    cull_report_free(&rep);
    free(r.ids);

    /* 比例 0.5（欠扩）：保留 {0,1}，相对真值漏剔 3 个 {3,4,6} */
    CK(cull_motion(hand_boxes, 7, v, 0.5, &r, &rep, &err) == CULL_OK);
    CK(r.count == 2 && has_id(&r, 0) && has_id(&r, 1));
    cull_report_free(&rep);
    free(r.ids);

    /* 比例 1.5（过扩）：保留 {0,1,3,4,5,6}，漏剔 0、误剔 1 个 {5} */
    CK(cull_motion(hand_boxes, 7, v, 1.5, &r, &rep, &err) == CULL_OK);
    CK(r.count == 6 && has_id(&r, 5));
    cull_report_free(&rep);
    free(r.ids);

    /* 分两阶段调用，结果与合一一致 */
    CullBox expanded[7];
    CK(cull_expand_boxes(hand_boxes, 7, 1.0, expanded, &rep, &err) == CULL_OK);
    CK(cull_run_swept(expanded, 7, v, &r) == CULL_OK);
    CK(r.count == 5);
    CK(fabs(expanded[1].x - 85.0) < 1e-12 && fabs(expanded[1].w - 50.0) < 1e-12);
    cull_report_free(&rep);
    free(r.ids);
}

static void test_errors_and_equivalence(void) {
    CullView v = {0, 0, 100, 100};
    CullResult r;
    CullMotionReport rep;
    int err = -99;

    /* 负速度 -> CULL_E_NEG，并指出下标（0 起）*/
    CullMovingBox neg[2] = {{0,0,10,10,0, 10,1,0}, {5,5,10,10,0, -3,1,0}};
    CK(cull_motion(neg, 2, v, 1.0, &r, &rep, &err) == CULL_E_NEG);
    CK(err == 1);  /* 第 2 个对象（序号 idx+1=2）*/
    cull_report_free(&rep);  /* 报错路径已分配的报告数组仍需释放 */

    /* 扩展为零：speed==0 -> CULL_E_ZERO */
    CullMovingBox stopped[2] = {{0,0,10,10,0, 5,1,0}, {9,9,10,10,0, 0,1,0}};
    CK(cull_motion(stopped, 2, v, 1.0, &r, &rep, &err) == CULL_E_ZERO);
    CK(err == 1);  /* 第 2 个 */
    cull_report_free(&rep);

    /* 扩展为零：零方向向量 -> CULL_E_ZERO */
    CullMovingBox nodir[1] = {{9,9,10,10,0, 8, 0, 0}};
    CK(cull_motion(nodir, 1, v, 1.0, &r, &rep, &err) == CULL_E_ZERO);
    CK(err == 0);  /* 第 1 个 */
    cull_report_free(&rep);

    /* NaN 输入：速度 NaN 按负值类拦截；方向 NaN 按扩展为零拦截；比例 NaN 为参数错误 */
    CullMovingBox nan_speed[1] = {{0,0,10,10,0, NAN, 1, 0}};
    CK(cull_motion(nan_speed, 1, v, 1.0, &r, &rep, &err) == CULL_E_NEG);
    CK(err == 0);
    cull_report_free(&rep);
    CullMovingBox nan_dir[1] = {{0,0,10,10,0, 8, NAN, 0}};
    CK(cull_motion(nan_dir, 1, v, 1.0, &r, &rep, &err) == CULL_E_ZERO);
    CK(err == 0);
    cull_report_free(&rep);
    CK(cull_motion(nodir, 1, v, NAN, &r, &rep, &err) == CULL_E_ARG);

    /* 不扩展（ratio<=0）：集合照旧，与 cull_run 逐对象一致；且不校验速度 */
    CullBox plain[2] = {{0,0,10,10,0}, {150,150,10,10,0}};
    CullResult base;
    CK(cull_run(plain, 2, v, &base) == 0);
    CullMovingBox weird[2] = {{0,0,10,10,0, -7, 0, 0}, {150,150,10,10,0, 0, 1, 0}};
    CK(cull_motion(weird, 2, v, 0.0, &r, &rep, &err) == CULL_OK);
    CK(same_set(&base, &r));
    free(r.ids);                       /* 两次调用之间释放，避免覆盖泄漏 */
    CK(cull_motion(weird, 2, v, -2.0, &r, &rep, &err) == CULL_OK);
    CK(same_set(&base, &r));
    free(base.ids);
    free(r.ids);

    /* 视口非法 */
    CullView bad = {0, 0, -1, 10};
    CK(cull_motion(hand_boxes, 7, bad, 1.0, &r, &rep, &err) == CULL_E_VIEW);
}

int main(void) {
    test_legacy();
    test_motion();
    test_errors_and_equivalence();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
