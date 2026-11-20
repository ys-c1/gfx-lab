#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 手算参考场景：光 (64.5,10.5) r=40，A: tau=0.5，B: tau=0.3 */
static void hand_scene(ShLight *l, ShSegT *segs) {
    *l = (ShLight){64.5f, 10.5f, 40.0f, 1.0f};
    segs[0] = (ShSegT){20.0f, 60.0f, 108.0f, 60.0f, 0.5f};
    segs[1] = (ShSegT){44.0f, 70.0f, 84.0f, 70.0f, 0.3f};
}

int main(void) {
    ShadowMap m;
    CK(sh_init(&m, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    CK(sh_render(&m, &l, NULL, 0) == 0);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == -2);
    sh_free(&m);

    /* 不透光时画面照旧：tau 全 0 必须与旧 sh_render 逐像素一致 */
    ShadowMap a, b;
    CK(sh_init(&a, 128, 128) == 0);
    CK(sh_init(&b, 128, 128) == 0);
    ShSegT top[2];
    ShLight hl;
    hand_scene(&hl, top);
    top[0].tau = 0.0f; top[1].tau = 0.0f;
    ShSeg old[2] = {{20.0f, 60.0f, 108.0f, 60.0f}, {44.0f, 70.0f, 84.0f, 70.0f}};
    CK(sh_render(&a, &hl, old, 2) == 0);
    CK(sh_render_t(&b, &hl, top, 2, NULL) == 0);
    CK(memcmp(a.buf, b.buf, (size_t)128 * 128 * sizeof(float)) == 0);

    /* tau=1 全透明：等于无遮挡渲染 */
    top[0].tau = 1.0f; top[1].tau = 1.0f;
    CK(sh_render(&a, &hl, NULL, 0) == 0);
    CK(sh_render_t(&b, &hl, top, 2, NULL) == 0);
    CK(memcmp(a.buf, b.buf, (size_t)128 * 128 * sizeof(float)) == 0);

    /* 透光率越界 / 遮挡段退化：报错并指出序号 */
    ShSegT badseg[2];
    hand_scene(&hl, badseg);
    badseg[1].tau = 1.5f;
    CK(sh_render_t(&b, &hl, badseg, 2, NULL) == SH_ERR_TAU(1));
    hand_scene(&hl, badseg);
    badseg[0].tau = -0.25f;
    CK(sh_render_t(&b, &hl, badseg, 2, NULL) == SH_ERR_TAU(0));
    hand_scene(&hl, badseg);
    badseg[1].x1 = badseg[1].x0; badseg[1].y1 = badseg[1].y0;
    CK(sh_render_t(&b, &hl, badseg, 2, NULL) == SH_ERR_DEGEN(1));

    /* 手算透光率比对：
       像素 (64,100)：竖直光线穿 A、B，T = 0.5*0.3 = 0.15，d = 90，
       亮度 = 1/(1+90/40) * 0.15 = 0.0461538...
       像素 (100,100)：只穿 A，T = 0.5，d = sqrt(36^2+90^2) = 96.9330，
       亮度 = 1/(1+96.9330/40) * 0.5 = 0.1460569... */
    hand_scene(&hl, badseg);
    ShStats st;
    CK(sh_render_t(&b, &hl, badseg, 2, &st) == 0);
    float hand_overlap = 1.0f / (1.0f + 90.0f / 40.0f) * 0.15f;
    float hand_single = 1.0f / (1.0f + sqrtf(9396.0f) / 40.0f) * 0.5f;
    CK(fabsf(b.buf[(size_t)100 * 128 + 64] - hand_overlap) < 1e-4f);
    CK(fabsf(b.buf[(size_t)100 * 128 + 100] - hand_single) < 1e-4f);

    /* 重叠区单独统计 */
    CK(st.n_overlap > 0);
    CK(st.n_single > 0);
    CK(st.n_lit > 0);
    CK(st.n_lit + st.n_single + st.n_overlap == 128 * 128);
    CK(st.mean_overlap < st.mean_single);
    CK(st.max_trans_err < 1e-6f);

    sh_free(&a);
    sh_free(&b);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
