#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f, 0.0f, 0.0f, 0.0f};   /* aw=ah=0：点光源 */
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);

    /* ---- 开关为假：OFF 档与 aw=ah=0 的输出必须和旧路径逐位一致 -------------- */
    const ShRenderCfg *off = sh_preset_cfg(SH_GEAR_OFF);
    CK(off && off->cols == 1 && off->rows == 1);
    ShadowMap ref, ex;
    CK(sh_init(&ref, 64, 64) == 0 && sh_init(&ex, 64, 64) == 0);
    float cov_off[64 * 64];
    CK(sh_render(&ref, &l, &s, 1) == 0);
    CK(sh_render_ex(&ex, cov_off, NULL, &l, off, &s, 1) == 0);
    int bit_same = 1;
    for (int i = 0; i < 64 * 64; ++i) if (ref.buf[i] != ex.buf[i]) { bit_same = 0; break; }
    CK(bit_same);
    /* 点光源路径的覆盖比例只能是 0 或 1。 */
    int cov_binary = 1;
    for (int i = 0; i < 64 * 64; ++i)
        if (cov_off[i] != 0.0f && cov_off[i] != 1.0f) { cov_binary = 0; break; }
    CK(cov_binary);
    /* 点光源没有过渡带，半影宽度为 0。 */
    ShPenumbra pen0;
    CK(sh_penumbra(cov_off, 64, 64, &pen0) == 0);
    CK(pen0.width_avg == 0.0f && pen0.width_max == 0.0f);

    /* ---- 第 1 段：采点数量、分布边界与朝向旋转 ----------------------------- */
    ShLight al = {32.0f, 40.0f, 30.0f, 1.0f, 4.0f, 4.0f, 0.0f};  /* 半宽/半高 4 的方形面光 */
    const ShRenderCfg *fast = sh_preset_cfg(SH_GEAR_FAST);
    const ShRenderCfg *slow = sh_preset_cfg(SH_GEAR_SLOW);
    CK(fast->cols * fast->rows == 16);
    CK(slow->cols * slow->rows == 144);
    ShPoint pts[256];
    int nfast = sh_sample_quad(&al, fast, pts);
    CK(nfast == 16);
    /* angle=0：所有采样点应落在以光源中心为中心、半宽 aw/半高 ah 的矩形内。 */
    int inside = 1;
    for (int i = 0; i < nfast; ++i) {
        float dx = pts[i].x - al.x, dy = pts[i].y - al.y;
        if (dx < -al.aw - 1e-5f || dx > al.aw + 1e-5f ||
            dy < -al.ah - 1e-5f || dy > al.ah + 1e-5f) inside = 0;
    }
    CK(inside);
    /* 局部坐标按行优先、居中分布；角点接近 (-aw,-ah)。 */
    float lx, ly;
    CK(sh_local_point(fast, 0, &lx, &ly) == 0);
    CK(fabsf(lx - (-0.75f)) < 1e-6f && fabsf(ly - (-0.75f)) < 1e-6f);
    CK(sh_local_point(fast, 15, &lx, &ly) == 0);
    CK(fabsf(lx - 0.75f) < 1e-6f && fabsf(ly - 0.75f) < 1e-6f);

    /* 朝向 90 度：angle=0 时的 (x,y) 点旋转后变为 (-y,x)（screen y-down）。 */
    ShLight al90 = al; al90.angle = (float)M_PI / 2.0f;
    ShPoint pts90[16];
    CK(sh_sample_quad(&al90, fast, pts90) == 16);
    int rot_ok = 1;
    for (int i = 0; i < 16; ++i) {
        float dx0 = pts[i].x - al.x, dy0 = pts[i].y - al.y;
        float dx1 = pts90[i].x - al90.x, dy1 = pts90[i].y - al90.y;
        if (fabsf(dx1 - (-dy0)) > 1e-5f || fabsf(dy1 - dx0) > 1e-5f) rot_ok = 0;
    }
    CK(rot_ok);
    /* 朝向只旋转、不改变采样点到中心的距离分布。 */
    float r0 = 0.0f, r1 = 0.0f;
    for (int i = 0; i < 16; ++i) {
        r0 += (pts[i].x-al.x)*(pts[i].x-al.x) + (pts[i].y-al.y)*(pts[i].y-al.y);
        r1 += (pts90[i].x-al90.x)*(pts90[i].x-al90.x) + (pts90[i].y-al90.y)*(pts90[i].y-al90.y);
    }
    CK(fabsf(r0 - r1) < 1e-4f);

    /* ---- 抖动采样：确定性可复现、不越界、且确实偏离规则网格 ----------------- */
    ShRenderCfg jcfg = *fast; jcfg.jitter = 0.5f;
    ShPoint jp[16], jp2[16];
    CK(sh_sample_quad(&al, &jcfg, jp) == 16);
    CK(sh_sample_quad(&al, &jcfg, jp2) == 16);
    int jitter_same = 1, jitter_moved = 0, jitter_inside = 1;
    for (int i = 0; i < 16; ++i) {
        if (jp[i].x != jp2[i].x || jp[i].y != jp2[i].y) jitter_same = 0;   /* 可复现 */
        if (jp[i].x != pts[i].x || jp[i].y != pts[i].y) jitter_moved = 1;  /* 有偏移 */
        float dx = jp[i].x - al.x, dy = jp[i].y - al.y;
        if (dx < -al.aw - 1e-5f || dx > al.aw + 1e-5f ||
            dy < -al.ah - 1e-5f || dy > al.ah + 1e-5f) jitter_inside = 0;
    }
    CK(jitter_same && jitter_moved && jitter_inside);
    ShRenderCfg badj = *fast; badj.jitter = 1.5f;
    CK(sh_sample_quad(&al, &badj, jp) == -1);

    /* ---- 冲突配置必须报错，且信息指明两方 ---------------------------------- */
    ShLight pl = {32.0f, 8.0f, 20.0f, 1.0f, 0.0f, 0.0f, 0.0f};  /* 点光源 */
    ShRenderCfg rc;
    /* 面光 + OFF 档 */
    rc = *off;
    CK(sh_render_ex(&ex, cov_off, NULL, &al, &rc, &s, 1) == -3);
    CK(strstr(sh_last_error(), "light.aw/ah") && strstr(sh_last_error(), "OFF"));
    /* 点光源 + FAST 档 */
    rc = *fast;
    CK(sh_render_ex(&ex, cov_off, NULL, &pl, &rc, &s, 1) == -3);
    CK(strstr(sh_last_error(), "aw=ah=0") && strstr(sh_last_error(), "gear=1"));
    /* 面光走旧接口 */
    CK(sh_render(&ex, &al, &s, 1) == -3);
    CK(strstr(sh_last_error(), "legacy sh_render"));
    /* 点光源 + 多点网格（即便 gear 被改成 OFF 也冲突） */
    ShRenderCfg weird = {SH_GEAR_OFF, 4, 4, 0.0f};
    CK(sh_render_ex(&ex, cov_off, NULL, &pl, &weird, &s, 1) == -3);
    CK(strstr(sh_last_error(), "multi sample"));
    /* 面光 + 单点网格 */
    ShRenderCfg single = {SH_GEAR_SLOW, 1, 1, 0.0f};
    CK(sh_render_ex(&ex, cov_off, NULL, &al, &single, &s, 1) == -3);
    CK(strstr(sh_last_error(), "single point"));
    /* 半轴只给一个（aw>0, ah=0）属非法光源。 */
    ShLight half = {32.0f, 8.0f, 20.0f, 1.0f, 4.0f, 0.0f, 0.0f};
    CK(sh_render_ex(&ex, cov_off, NULL, &half, fast, &s, 1) == -2);

    /* ---- 第 2/3 段：覆盖比例合成与半影 ------------------------------------- */
    /* 面光在上方 y=8，横墙 y=30：墙后 y=60 为本影，光源同侧 y=2 为全亮。
     * 用 aw!=ah 的矩形面光，朝向才有可观测的几何意义。 */
    ShLight scene = {32.0f, 8.0f, 30.0f, 1.0f, 6.0f, 2.0f, 0.0f};
    ShSeg wall = {10.0f, 30.0f, 54.0f, 30.0f};
    float cov[64 * 64];
    ShPenumbra pen;
    CK(sh_render_ex(&ex, cov, &pen, &scene, slow, &wall, 1) == 0);
    /* 覆盖比例取值必须落在 [0,1]，且本影深处为 0、远离遮挡物处为 1。 */
    int range_ok = 1, has_partial = 0;
    for (int i = 0; i < 64 * 64; ++i) {
        if (cov[i] < -1e-6f || cov[i] > 1.0f + 1e-6f) range_ok = 0;
        if (cov[i] > 1e-4f && cov[i] < 1.0f - 1e-4f) has_partial = 1;
    }
    CK(range_ok);
    CK(has_partial);                              /* 面光必须产生软过渡 */
    CK(cov[(size_t)60 * 64 + 32] < 0.05f);        /* 墙后深处：本影 */
    CK(cov[(size_t)2 * 64 + 32] > 0.99f);         /* 光源同侧：全亮 */
    CK(pen.width_max > 0.0f && pen.transitions > 0);
    /* 面光越大半影越宽。 */
    ShLight big = scene; big.aw = 12.0f; big.ah = 4.0f;
    float cov_big[64 * 64]; ShPenumbra pen_big;
    CK(sh_render_ex(&ex, cov_big, &pen_big, &big, slow, &wall, 1) == 0);
    CK(pen_big.width_max >= pen.width_max);

    /* ---- 与穷举结果核对：两档都应逼近 48x48 穷举参考 ------------------------ */
    const ShRenderCfg *exh = sh_preset_cfg(SH_GEAR_EXHAUST);
    CK(exh->cols * exh->rows == 2304);
    float cov_ref[64 * 64], cov_fast[64 * 64], cov_slow[64 * 64];
    ShadowMap mref, mfast, mslow;
    CK(sh_init(&mref, 64, 64) == 0 && sh_init(&mfast, 64, 64) == 0 && sh_init(&mslow, 64, 64) == 0);
    CK(sh_render_ex(&mref, cov_ref, NULL, &scene, exh, &wall, 1) == 0);
    CK(sh_render_ex(&mfast, cov_fast, NULL, &scene, fast, &wall, 1) == 0);
    CK(sh_render_ex(&mslow, cov_slow, NULL, &scene, slow, &wall, 1) == 0);
    double se_fast = 0, se_slow = 0;
    for (int i = 0; i < 64 * 64; ++i) {
        double ef = cov_fast[i] - cov_ref[i], es = cov_slow[i] - cov_ref[i];
        se_fast += ef * ef; se_slow += es * es;
    }
    double rmse_fast = sqrt(se_fast / (64.0 * 64.0));
    double rmse_slow = sqrt(se_slow / (64.0 * 64.0));
    printf("coverage RMSE vs exhaustive: fast=%.5f slow=%.5f\n", rmse_fast, rmse_slow);
    CK(rmse_fast < 0.20);                         /* 16 点档误差有界 */
    CK(rmse_slow < rmse_fast);                    /* 144 点档严格更准 */
    CK(rmse_slow < 0.08);
    /* 穷举档自身的半影与低速档接近。 */
    ShPenumbra pen_ref, pen_fast;
    CK(sh_penumbra(cov_ref, 64, 64, &pen_ref) == 0);
    CK(sh_penumbra(cov_fast, 64, 64, &pen_fast) == 0);
    CK(fabsf(pen_ref.width_avg - pen.width_avg) <= 1.0f);
    CK(fabsf(pen_fast.width_avg - pen_ref.width_avg) <= 2.0f);

    /* ---- 各朝向都能完整渲染，且结果有差异（朝向确实生效） ------------------- *
     * 矩形面光（aw!=ah）+ 偏离光轴的短墙：旋转发光面时采样点集真正改变，       *
     * 不同朝向的覆盖比例之和应当不同。正方形有 90 度对称，不能用来检验朝向。 */
    ShSeg offwall = {36.0f, 30.0f, 54.0f, 30.0f};
    float cov_ang[64 * 64]; ShadowMap mang;
    CK(sh_init(&mang, 64, 64) == 0);
    float prev_sum = -1.0f; int ang_diff = 1;
    const float angles[] = {0.0f, (float)M_PI/4, (float)M_PI/2, (float)M_PI};
    for (int a = 0; a < 4; ++a) {
        ShLight la = scene; la.angle = angles[a];
        CK(sh_render_ex(&mang, cov_ang, NULL, &la, fast, &offwall, 1) == 0);
        float sum = 0.0f;
        for (int i = 0; i < 64 * 64; ++i) sum += cov_ang[i];
        if (a > 0 && fabsf(sum - prev_sum) < 1e-3f) ang_diff = 0;
        prev_sum = sum;
    }
    CK(ang_diff);

    sh_free(&m); sh_free(&ref); sh_free(&ex);
    sh_free(&mref); sh_free(&mfast); sh_free(&mslow); sh_free(&mang);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
