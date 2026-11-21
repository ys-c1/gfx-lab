#include "cull.h"
#include "shake.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);

    /* 第一段：按抖动幅度外扩容差，按档取整 */
    ShakeCfg cfg = {1, 2.0, 0.5, 10.0, 4}; /* 档宽 2.5 */
    double tol = 0.0; int tier = 0;
    CK(shake_tolerance(&cfg, 0.0, &tol, &tier) == 0);
    CK(tier == 1 && fabs(tol - 2.5) < 1e-9);
    CK(shake_tolerance(&cfg, 8.0, &tol, &tier) == 0); /* raw = 2 + 4 = 6 */
    CK(tier == 3 && fabs(tol - 7.5) < 1e-9);
    CK(shake_tolerance(&cfg, 16.0, &tol, &tier) == 0); /* raw = 10 = 上限，不超限 */
    CK(tier == 4 && fabs(tol - 10.0) < 1e-9);

    /* 超限：报错并给出上限值 */
    CK(shake_tolerance(&cfg, 16.5, &tol, &tier) == -3);
    ShakeFrame f;
    CK(shake_cull_frame(&cfg, b, 4, v, 100.0, &f, &r) == -3);
    CK(f.limit_exceeded == 1);
    CK(f.limit_value == 10.0);

    /* 关闭新分支：行为与输出照旧 */
    ShakeCfg off = cfg; off.enabled = 0;
    CK(shake_cull_frame(&off, b, 4, v, 5.0, &f, &r) == 0);
    CK(r.count == 3 && r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(f.candidates == 3 && f.ref_count == 3 && f.error == 0 && f.tier == 0);
    free(r.ids);

    /* 两段式正常帧：候选不少于参考工具，误差自洽 */
    CK(shake_cull_frame(&cfg, b, 4, v, 4.0, &f, &r) == 0);
    CK(f.ref_count == 3);
    CK(f.candidates >= f.ref_count);
    CK(f.error == f.candidates - f.ref_count);
    CK(f.tier >= 1 && f.tier <= cfg.tiers);
    free(r.ids);

    /* 汇总：负载方差与峰谷差 */
    ShakeFrame fs[3] = {0};
    fs[0].candidates = 2; fs[1].candidates = 4; fs[2].candidates = 6;
    fs[1].error = 1; fs[2].error = 2;
    ShakeStats st;
    shake_stats(fs, 3, &st);
    CK(st.frames == 3);
    CK(fabs(st.load_mean - 4.0) < 1e-9);
    CK(fabs(st.load_variance - 8.0 / 3.0) < 1e-9);
    CK(st.load_peak == 6 && st.load_valley == 2 && st.peak_valley_diff == 4);
    CK(st.total_error == 3);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
