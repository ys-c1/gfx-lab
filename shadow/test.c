#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* scene used by the animation checks */
static const ShSeg SEGS[3] = {
    {20.0f, 40.0f, 80.0f, 44.0f},
    {70.0f, 20.0f, 74.0f, 90.0f},
    {10.0f, 100.0f, 110.0f, 96.0f},
};

static int same_map(const ShadowMap *a, const ShadowMap *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    for (int i = 0; i < a->w * a->h; ++i)
        if (a->buf[i] != b->buf[i]) return 0;
    return 1;
}

static void test_legacy_unchanged(void) {
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
}

static void test_lerp(void) {
    ShLight a = {10.0f, 20.0f, 30.0f, 0.5f};
    ShLight b = {30.0f, 40.0f, 10.0f, 1.5f};
    ShLight o;
    ShError e;

    CK(sh_lerp_light(&a, &b, 0.0f, 1, &o, &e) == SH_OK);
    CK(o.x == 10.0f && o.y == 20.0f && o.radius == 30.0f && o.intensity == 0.5f);
    CK(sh_lerp_light(&a, &b, 1.0f, 1, &o, &e) == SH_OK);
    CK(o.x == 30.0f && o.y == 40.0f && o.radius == 10.0f && o.intensity == 1.5f);

    CK(sh_lerp_light(&a, &b, 0.25f, 1, &o, &e) == SH_OK);
    CK(fabsf(o.x - 15.0f) < 1e-5f);
    CK(fabsf(o.y - 25.0f) < 1e-5f);
    CK(fabsf(o.radius - 25.0f) < 1e-5f);
    CK(fabsf(o.intensity - 0.75f) < 1e-5f);

    /* t out of range names "t" */
    e.code = SH_OK; e.param = NULL; e.msg[0] = 0;
    CK(sh_lerp_light(&a, &b, 1.5f, 1, &o, &e) == SH_ERR_T_RANGE);
    CK(e.param && strcmp(e.param, "t") == 0);
    CK(strstr(e.msg, "t=") != NULL);
    CK(sh_lerp_light(&a, &b, -0.01f, 1, &o, &e) == SH_ERR_T_RANGE);
    CK(e.param && strcmp(e.param, "t") == 0);
    CK(sh_lerp_light(&a, &b, NAN, 1, &o, &e) == SH_ERR_T_RANGE);

    /* zero frame interval names "frame_span" */
    CK(sh_lerp_light(&a, &b, 0.5f, 0, &o, &e) == SH_ERR_FRAME_SPAN);
    CK(e.param && strcmp(e.param, "frame_span") == 0);
    CK(sh_lerp_light(&a, &b, 0.5f, -3, &o, &e) == SH_ERR_FRAME_SPAN);
    CK(e.param && strcmp(e.param, "frame_span") == 0);

    /* NULL */
    CK(sh_lerp_light(NULL, &b, 0.5f, 1, &o, &e) == SH_ERR_NULL);
    CK(e.param && strcmp(e.param, "prev") == 0);
    CK(sh_lerp_light(&a, &b, 0.5f, 1, NULL, &e) == SH_ERR_NULL);
    CK(e.param && strcmp(e.param, "out") == 0);

    /* invalid light */
    ShLight bad = {1.0f, 2.0f, 0.0f, 1.0f};
    CK(sh_lerp_light(&bad, &b, 0.5f, 1, &o, &e) == SH_ERR_LIGHT);
    CK(e.param && strcmp(e.param, "prev") == 0);
}

/* Interpolation off must reproduce the legacy full render bit-for-bit. */
static void test_interp_off_matches_legacy(void) {
    const int W = 96, H = 96;
    ShadowMap ref;
    ShAnim a;
    CK(sh_init(&ref, W, H) == 0);
    CK(sh_anim_init(&a, W, H, 16) == SH_OK);
    CK(sh_anim_set_scene(&a, SEGS, 3) == SH_OK);

    ShLight prev = {10.0f, 10.0f, 30.0f, 1.0f};
    ShLight next = {80.0f, 12.0f, 30.0f, 0.8f};
    ShStepStats st;
    ShError e;

    /* Step 1, interpolation off. */
    CK(sh_render(&ref, &next, SEGS, 3) == 0);
    CK(sh_anim_step(&a, &prev, &next, 0.37f, 5, 0, &st, &e) == SH_OK);
    CK(same_map(&ref, &a.map));
    CK(st.full_refresh == 1);
    CK(st.interpolated == 0);
    CK(st.dirty_blocks == st.total_blocks);

    /* Step 2 with interpolation off and a different light, still full. */
    ShLight third = {50.0f, 5.0f, 25.0f, 1.2f};
    CK(sh_render(&ref, &third, SEGS, 3) == 0);
    CK(sh_anim_step(&a, &next, &third, 0.9f, 4, 0, &st, &e) == SH_OK);
    CK(same_map(&ref, &a.map));
    CK(st.full_refresh == 1);

    /* Step with t outside [0,1] must still be accepted when disabled. */
    CK(sh_anim_step(&a, &third, &third, 3.0f, 0, 0, &st, &e) == SH_OK);

    sh_anim_free(&a);
    sh_free(&ref);
}

/* Incremental vs. a from-scratch render of the same interpolated light. */
static void test_incremental_accuracy(void) {
    const int W = 128, H = 128, FRAMES = 64;
    ShadowMap ref;
    ShAnim a;
    CK(sh_init(&ref, W, H) == 0);
    CK(sh_anim_init(&a, W, H, 8) == SH_OK);
    CK(sh_anim_set_scene(&a, SEGS, 3) == SH_OK);

    ShLight prev = {10.0f, 6.0f, 40.0f, 1.0f};
    ShLight next = {118.0f, 14.0f, 40.0f, 1.0f};
    int total_dirty = 0, steps = 0;
    double worst = 0.0;

    for (int f = 0; f < FRAMES; ++f) {
        float t = (float)f / (float)(FRAMES - 1);
        ShLight lit;
        ShStepStats st;
        ShError e;
        CK(sh_lerp_light(&prev, &next, t, FRAMES - 1, &lit, &e) == SH_OK);
        CK(sh_render(&ref, &lit, SEGS, 3) == 0);
        CK(sh_anim_step(&a, &prev, &next, t, FRAMES - 1, 1, &st, &e) == SH_OK);
        double mx = 0.0;
        CK(sh_compare_maps(&ref, &a.map, &mx, NULL, NULL) == SH_OK);
        if (mx > worst) worst = mx;
        CK(f >= 1 ? st.full_refresh == 0 : st.full_refresh == 1);
        total_dirty += st.dirty_blocks;
        ++steps;
    }
    /* The per-block bound promises error <= eps = 1/255 on unchanged blocks;
     * a pixel that just flips visibility is allowed one step of slack. */
    CK(worst <= 1.0 / 255.0 + 1e-4);
    printf("  incremental: frames=%d avg dirty blocks %.1f/256, max err %.6f\n",
           steps, (double)total_dirty / (double)steps, worst);

    sh_anim_free(&a);
    sh_free(&ref);
}

/* Exact invariant behind the incremental renderer: after every step the
 * cached visibility mask must match a fresh occlusion test for the current
 * light.  A mismatch here is a visibly wrong shadow, not a rounding issue. */
static int blocked_ref(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static long mask_mismatches(const ShAnim *a, const ShLight *l) {
    long bad = 0;
    for (int y = 0; y < a->map.h; ++y)
        for (int x = 0; x < a->map.w; ++x) {
            int want = 0;
            for (int i = 0; i < a->nseg && !want; ++i)
                want = blocked_ref(&a->segs[i], l->x, l->y, (float)x + 0.5f, (float)y + 0.5f);
            if ((int)a->occ[(size_t)y * a->map.w + x] != want) ++bad;
        }
    return bad;
}

static void run_mask_case(const char *name, int W, int H, int block, int frames,
                          const ShSeg *segs, int nseg, ShLight p, ShLight q) {
    ShAnim a;
    CK(sh_anim_init(&a, W, H, block) == SH_OK);
    CK(sh_anim_set_scene(&a, segs, nseg) == SH_OK);
    long bad = 0;
    for (int f = 0; f < frames; ++f) {
        float t = (float)f / (float)(frames - 1);
        ShStepStats st;
        ShError e;
        ShLight cur;
        CK(sh_anim_step(&a, &p, &q, t, frames - 1, 1, &st, &e) == SH_OK);
        CK(sh_lerp_light(&p, &q, t, frames - 1, &cur, &e) == SH_OK);
        bad += mask_mismatches(&a, &cur);
    }
    if (bad) printf("  %s: %ld mask mismatches\n", name, bad);
    CK(bad == 0);
    sh_anim_free(&a);
}

static void test_mask_exactness(void) {
    ShLight p = {10.0f, 6.0f, 40.0f, 1.0f};
    ShLight q = {118.0f, 14.0f, 40.0f, 1.0f};
    run_mask_case("scene 128/8", 128, 128, 8, 96, SEGS, 3, p, q);
    run_mask_case("scene 128/4", 128, 128, 4, 96, SEGS, 3, p, q);
    run_mask_case("scene 512/16", 512, 512, 16, 48, SEGS, 3, p, q);

    /* Light path passes right through a blocker segment. */
    ShSeg one[1] = {{60.0f, 0.0f, 64.0f, 128.0f}};
    ShLight cp = {20.0f, 60.0f, 30.0f, 1.0f};
    ShLight cq = {110.0f, 70.0f, 30.0f, 1.0f};
    run_mask_case("crossing", 128, 128, 8, 64, one, 1, cp, cq);

    /* Light path passes through a blocker endpoint, and a tiny move. */
    ShSeg ep[1] = {{64.0f, 64.0f, 120.0f, 20.0f}};
    ShLight ep0 = {10.0f, 64.0f, 30.0f, 1.0f};
    ShLight ep1 = {118.0f, 64.0f, 30.0f, 1.0f};
    run_mask_case("endpoint", 128, 128, 8, 64, ep, 1, ep0, ep1);

    ShLight sp0 = {10.0f, 10.0f, 25.0f, 1.0f};
    ShLight sp1 = {11.0f, 10.2f, 25.0f, 1.0f};
    run_mask_case("subpixel", 128, 128, 8, 128, SEGS, 3, sp0, sp1);

    /* Dense scene. */
    ShSeg dense[12];
    for (int i = 0; i < 12; ++i) {
        dense[i].x0 = 8.0f + (float)i * 9.0f;
        dense[i].y0 = 20.0f + (float)((i * 37) % 90);
        dense[i].x1 = 8.0f + (float)i * 9.0f;
        dense[i].y1 = 110.0f - (float)((i * 23) % 80);
    }
    ShLight dp = {5.0f, 5.0f, 50.0f, 1.0f};
    ShLight dq = {122.0f, 118.0f, 50.0f, 1.0f};
    run_mask_case("dense", 128, 128, 8, 64, dense, 12, dp, dq);

    /* No blockers. */
    run_mask_case("empty", 64, 64, 8, 32, NULL, 0, p, q);
}

static void test_anim_validation(void) {
    ShAnim a;
    ShLight p = {1.0f, 1.0f, 10.0f, 1.0f};
    ShLight q = {50.0f, 50.0f, 10.0f, 1.0f};
    ShStepStats st;
    ShError e;
    CK(sh_anim_init(&a, 32, 32, 8) == SH_OK);
    CK(sh_anim_set_scene(&a, SEGS, 3) == SH_OK);

    /* Interpolated step: bad t and bad span are rejected with the parameter
     * name in the message. */
    CK(sh_anim_step(&a, &p, &q, 2.0f, 1, 1, &st, &e) == SH_ERR_T_RANGE);
    CK(e.param && strcmp(e.param, "t") == 0);
    CK(sh_anim_step(&a, &p, &q, 0.5f, 0, 1, &st, &e) == SH_ERR_FRAME_SPAN);
    CK(e.param && strcmp(e.param, "frame_span") == 0);
    CK(sh_anim_step(&a, NULL, &q, 0.5f, 1, 1, &st, &e) == SH_ERR_NULL);
    CK(e.param && strcmp(e.param, "prev") == 0);
    CK(sh_anim_step(&a, &p, NULL, 0.5f, 1, 1, &st, &e) == SH_ERR_NULL);
    CK(e.param && strcmp(e.param, "next") == 0);
    CK(sh_anim_step(NULL, &p, &q, 0.5f, 1, 1, &st, &e) == SH_ERR_NULL);

    /* Nothing was consumed by the failed calls. */
    CK(a.frame == 0);
    CK(sh_anim_step(&a, &p, &q, 0.5f, 1, 1, &st, &e) == SH_OK);
    CK(a.frame == 1);

    CK(sh_anim_set_eps(&a, -1.0f) == SH_ERR_ARG);
    CK(sh_anim_set_eps(&a, 0.01f) == SH_OK);
    CK(sh_anim_init(&a, 0, 32, 8) == SH_ERR_ARG);
    sh_anim_free(&a);
    CK(a.map.buf == NULL && a.dirty == NULL);
}

static void test_trajectory(void) {
    ShLight p = {0.0f, 0.0f, 10.0f, 1.0f};
    ShLight q = {100.0f, 50.0f, 20.0f, 2.0f};
    ShError e;
    CK(sh_write_trajectory("anim_traj.csv", &p, &q, 11, 10, &e) == SH_OK);
    FILE *f = fopen("anim_traj.csv", "rb");
    CK(f != NULL);
    if (f) {
        char line[256];
        int lines = 0, bad = 0;
        double first_x = 0, last_x = 0, prev_t = -1, prev_fr = -1;
        while (fgets(line, sizeof line, f)) {
            ++lines;
            if (lines == 1) {
                CK(strncmp(line, "frame,t,x,y,radius,intensity", 27) == 0);
                continue;
            }
            int fr;
            double t, x, y, r, in;
            if (sscanf(line, "%d,%lf,%lf,%lf,%lf,%lf", &fr, &t, &x, &y, &r, &in) != 6) {
                ++bad;
                continue;
            }
            if (t <= prev_t || fr < prev_fr) ++bad;
            if (lines == 2) first_x = x;
            last_x = x;
            prev_t = t; prev_fr = fr;
        }
        CK(bad == 0);
        CK(lines == 12);                 /* header + 11 samples */
        CK(first_x == 0.0 && last_x == 100.0);
        fclose(f);
    }
    remove("anim_traj.csv");

    CK(sh_write_trajectory("x.csv", &p, &q, 1, 10, &e) == SH_ERR_ARG);
    CK(sh_write_trajectory("x.csv", &p, &q, 10, 0, &e) == SH_ERR_FRAME_SPAN);
    CK(sh_write_trajectory(NULL, &p, &q, 10, 1, &e) == SH_ERR_NULL);
}

int main(void) {
    test_legacy_unchanged();
    test_lerp();
    test_interp_off_matches_legacy();
    test_incremental_accuracy();
    test_mask_exactness();
    test_anim_validation();
    test_trajectory();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
