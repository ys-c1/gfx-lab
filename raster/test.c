#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Hand-computed expected image for a sprite centered at (8,8) with screen
   size 3 on a 16x16 black target: the square covers [6.5,9.5] x [6.5,9.5],
   so pixel coverage along each axis is 0.5, 1, 1, 0.5 for pixels 6..9. */
static void expect_size3(unsigned char *exp /* 16*16*3 */, unsigned char r,
                         unsigned char g, unsigned char b) {
    static const double cov[4] = { 0.5, 1.0, 1.0, 0.5 };
    memset(exp, 0, 16 * 16 * 3);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            double c = cov[j] * cov[i];
            unsigned char *p = exp + ((6 + j) * 16 + (6 + i)) * 3;
            p[0] = (unsigned char)(c * r + 0.5);
            p[1] = (unsigned char)(c * g + 0.5);
            p[2] = (unsigned char)(c * b + 0.5);
        }
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void) {
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    /* point sprites: scaling off matches the hand-computed image */
    unsigned char exp[16 * 16 * 3];
    expect_size3(exp, 200, 100, 50);
    RTarget a;
    CK(rt_init(&a, 16, 16) == 0);
    RSprite fixed = { 8.0, 8.0, 1.0, 3.0, {200, 100, 50} };
    CK(rt_sprites(&a, &fixed, 1, 0.0, NULL) == 1);
    CK(memcmp(a.rgb, exp, sizeof exp) == 0);
    CK(a.z[(7 * 16 + 7)] == 1.0f);

    /* perspective on, depth chosen so screen size is again 3: same image */
    RTarget b;
    CK(rt_init(&b, 16, 16) == 0);
    RSprite persp = { 8.0, 8.0, 8.0, 3.0, {200, 100, 50} };
    CK(rt_sprites(&b, &persp, 1, 8.0, NULL) == 1);
    CK(memcmp(b.rgb, exp, sizeof exp) == 0);

    /* nearer sprite is larger: z=2, focal=8, size=3 -> screen size 12,
       covering pixels 2..13 in both axes with full coverage */
    RTarget c;
    CK(rt_init(&c, 16, 16) == 0);
    RSprite near = { 8.0, 8.0, 2.0, 3.0, {200, 100, 50} };
    CK(rt_sprites(&c, &near, 1, 8.0, NULL) == 1);
    int full = 0;
    for (int i = 0; i < 16 * 16; ++i)
        if (c.rgb[i * 3] == 200 && c.rgb[i * 3 + 1] == 100 && c.rgb[i * 3 + 2] == 50)
            ++full;
    CK(full == 144);

    /* scaling off ignores depth: same footprint at any z */
    RTarget d;
    CK(rt_init(&d, 16, 16) == 0);
    RSprite far_fixed = { 8.0, 8.0, 100.0, 3.0, {200, 100, 50} };
    CK(rt_sprites(&d, &far_fixed, 1, 0.0, NULL) == 1);
    CK(memcmp(d.rgb, exp, sizeof exp) == 0);
    rt_free(&a); rt_free(&b); rt_free(&c); rt_free(&d);

    /* errors: zero size and non-finite depth report the sprite index */
    RTarget e;
    CK(rt_init(&e, 16, 16) == 0);
    int bad = -1;
    RSprite badsize[2] = {
        { 4.0, 4.0, 1.0, 2.0, {1, 2, 3} },
        { 8.0, 8.0, 1.0, 0.0, {1, 2, 3} },
    };
    CK(rt_sprites(&e, badsize, 2, 8.0, &bad) == -1 && bad == 1);
    RSprite badz[3] = {
        { 4.0, 4.0, 1.0, 2.0, {1, 2, 3} },
        { 8.0, 8.0, NAN, 2.0, {1, 2, 3} },
        { 12.0, 12.0, INFINITY, 2.0, {1, 2, 3} },
    };
    bad = -1;
    CK(rt_sprites(&e, badz, 3, 8.0, &bad) == -1 && bad == 1);
    /* non-finite depth is not an error while scaling is off */
    CK(rt_sprites(&e, badz, 3, 0.0, &bad) == 3);
    rt_free(&e);

    /* size tiers: area error, covered pixels, timing */
    const double tiers[] = { 64.0, 256.0 };
    const int ntiers = (int)(sizeof tiers / sizeof tiers[0]);
    printf("sprite size tiers: %d (", ntiers);
    for (int i = 0; i < ntiers; ++i) printf("%s%.0f", i ? " " : "", tiers[i]);
    printf(")\n");
    for (int i = 0; i < ntiers; ++i) {
        double ss = tiers[i];
        RTarget m;
        CK(rt_init(&m, 320, 320) == 0);
        RSprite s = { 160.3, 160.6, 0.5, ss, {255, 255, 255} };
        CK(rt_sprites(&m, &s, 1, 0.0, NULL) == 1);
        long sum = 0, covered = 0;
        for (int p = 0; p < 320 * 320; ++p)
            if (m.rgb[p * 3]) { sum += m.rgb[p * 3]; ++covered; }
        double area = (double)sum / 255.0;
        double err = fabs(area - ss * ss) / (ss * ss) * 100.0;
        const int reps = 50;
        double t0 = now_ms();
        for (int r = 0; r < reps; ++r) {
            s.z = 0.5 - (r + 1) * 1e-9;
            CK(rt_sprites(&m, &s, 1, 0.0, NULL) == 1);
        }
        double ms = (now_ms() - t0) / reps;
        printf("tier %.0f: area_err=%.4f%% covered=%ld time=%.3fms\n",
               ss, err, covered, ms);
        CK(err < 1.0);
        rt_free(&m);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
