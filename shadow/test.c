#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; \
    printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* ---- the renderer as it was before offsets, for the identity check -------- */
static int old_blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}
static void old_render(float *buf, int w, int h, const ShLight *l,
                       const ShSeg *segs, int nseg) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (old_blocked(&segs[i], l->x, l->y, px, py)) {
                    shadowed = 1;
                    break;
                }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - l->x, dy = py - l->y;
                float d = sqrtf(dx * dx + dy * dy);
                v = l->intensity / (1.0f + d / l->radius);
            }
            buf[(size_t)y * w + x] = v;
        }
}

/* ---- hand computation of one pixel, in double, from the definition ------- */
/* Distance from p to the segment along the light ray, or -1 when the ray
 * misses.  Written independently of the library on purpose. */
static double hand_hit_distance(const ShSeg *s, double lx, double ly,
                                double px, double py) {
    double rx = px - lx, ry = py - ly;
    double sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    double den = rx * sy - ry * sx;
    if (fabs(den) < 1e-12) return -1.0;
    double t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    double u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    if (!(t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0)) return -1.0;
    return (1.0 - t) * hypot(rx, ry);
}

/* ---- shared scene: same world at 128 and at 512 -------------------------- */
static const ShSeg k_seg = {20.0f, 30.0f, 110.0f, 50.0f};  /* slope 2/9 */
static const ShLight k_light = {65.0f, 2.0f, 60.0f, 1.0f};

static void scaled(float scale, ShSeg *seg, ShLight *light) {
    seg->x0 = k_seg.x0 * scale; seg->y0 = k_seg.y0 * scale;
    seg->x1 = k_seg.x1 * scale; seg->y1 = k_seg.y1 * scale;
    light->x = k_light.x * scale; light->y = k_light.y * scale;
    light->radius = k_light.radius * scale;
    light->intensity = k_light.intensity;
}

typedef struct {
    int stripes;       /* runs of self-shadowed surface pixels */
    int stripe_pixels; /* self-shadowed surface pixels */
    float offset_px;   /* configured offset, in pixels */
    float needed_px;   /* offset that would clear every surface pixel */
    float error_px;    /* offset_px - needed_px, positive means over-biased */
    double ms;         /* time to render */
} ShReport;

static int measure(int W, float scale, float bias, ShReport *r) {
    ShadowMap m;
    ShSeg seg;
    ShLight light;
    ShAcne acne;
    ShError err;
    clock_t t0, t1;
    scaled(scale, &seg, &light);
    if (sh_init_scaled(&m, W, W, scale) != 0) return -1;
    if (sh_set_bias(&m, bias, SH_DEFAULT_SLOPE_CLAMP, &err) != 0) {
        sh_free(&m);
        return -1;
    }
    t0 = clock();
    if (sh_render_ex(&m, &light, &seg, 1, &err) != 0) { sh_free(&m); return -1; }
    t1 = clock();
    if (sh_acne(&m, &light, &seg, 1, &acne, &err) != 0) { sh_free(&m); return -1; }
    r->stripes = acne.stripes;
    r->stripe_pixels = acne.shadowed;
    r->offset_px = sh_seg_offset(&m, &seg, 0) / scale;
    if (sh_seg_needed_offset(&m, &light, &seg, 0, &r->needed_px, &err) != 0) {
        sh_free(&m);
        return -1;
    }
    r->needed_px /= scale;
    r->error_px = r->offset_px - r->needed_px;
    r->ms = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
    sh_free(&m);
    return 0;
}

int main(void) {
    ShadowMap m;
    ShError err;

    /* ---- stage 1: offset from the surface slope ------------------------- */
    {
        float slope = -1.0f;
        CK(sh_seg_slope(&k_seg, 0, &slope, &err) == 0);
        CK(fabsf(slope - 20.0f / 90.0f) < 1e-6f);
        CK(err.field[0] == '\0');

        CK(sh_init(&m, 64, 64) == 0);
        CK(sh_set_bias(&m, 2.0f, 1.0f, &err) == 0);
        CK(fabsf(sh_seg_offset(&m, &k_seg, 0) - 2.0f * (20.0f / 90.0f)) < 1e-6f);
        /* The offset scales with world-units-per-pixel, so the per-pixel
         * offset is the same at either resolution of the same world. */
        {
            ShadowMap big;
            ShSeg seg;
            ShLight light;
            ShReport a, b;
            CK(sh_init_scaled(&big, 512, 512, 0.25f) == 0);
            scaled(0.25f, &seg, &light);
            CK(sh_set_bias(&big, 2.0f, 1.0f, &err) == 0);
            CK(fabsf(sh_seg_offset(&big, &seg, 0) / 0.25f -
                     sh_seg_offset(&m, &k_seg, 0)) < 1e-5f);
            sh_free(&big);
            CK(measure(128, 1.0f, 2.0f, &a) == 0);
            CK(measure(512, 0.25f, 2.0f, &b) == 0);
            CK(fabsf(a.offset_px - b.offset_px) < 1e-4f);
        }
        /* A horizontal surface needs no offset. */
        {
            ShSeg flat = {10.0f, 30.0f, 60.0f, 30.0f};
            CK(sh_seg_offset(&m, &flat, 0) == 0.0f);
        }
        /* Zero bias is exactly the old renderer. */
        {
            float old_buf[64 * 64];
            ShLight light = {32.0f, 8.0f, 20.0f, 1.0f};
            ShSeg segs[2] = {{10.0f, 20.0f, 54.0f, 44.0f},
                             {20.0f, 50.0f, 50.0f, 20.0f}};
            CK(sh_set_bias(&m, 0.0f, 1.0f, &err) == 0);
            CK(sh_render(&m, &light, segs, 2) == 0);
            old_render(old_buf, 64, 64, &light, segs, 2);
            CK(memcmp(old_buf, m.buf, sizeof old_buf) == 0);
        }
        sh_free(&m);
    }

    /* ---- stage 2: the offset inside the intersection -------------------- */
    /* Per-pixel comparison against a hand computation, double precision and
     * written independently of the library. */
    {
        ShSeg segs[2] = {{20.0f, 30.0f, 110.0f, 50.0f},
                         {30.0f, 80.0f, 100.0f, 70.0f}};
        ShLight light = {65.0f, 2.0f, 60.0f, 1.0f};
        float off[2];
        CK(sh_init(&m, 128, 128) == 0);
        CK(sh_set_bias(&m, SH_DEFAULT_BIAS, SH_DEFAULT_SLOPE_CLAMP, &err) == 0);
        off[0] = sh_seg_offset(&m, &segs[0], 0);
        off[1] = sh_seg_offset(&m, &segs[1], 1);
        printf("hand check, offset %.4f and %.4f world units:\n", off[0], off[1]);
        for (int y = 0; y < 128; y += 17) {
            for (int x = 0; x < 128; x += 19) {
                double px = x + 0.5, py = y + 0.5;
                int expect = 0;
                for (int i = 0; i < 2; ++i) {
                    double d = hand_hit_distance(&segs[i], light.x, light.y, px, py);
                    if (d >= 0.0 && d >= (double)off[i]) expect = 1;
                }
                int got = sh_pixel_blocked(&m, &light, segs, 2, x, y);
                if (got != expect) {
                    printf("  pixel %d,%d: hand %d, library %d\n", x, y, expect, got);
                    ++fails;
                }
                ++checks;
            }
        }
        /* Known answer: pixels on the far side of the line stay lit even
         * though the plain ray touches the segment. */
        {
            double px = 64.5, py = 39.9; /* just under seg0's line */
            double d = hand_hit_distance(&segs[0], light.x, light.y, px, py);
            CK(d >= 0.0 && d < (double)off[0]);
            CK(sh_pixel_blocked(&m, &light, segs, 2, 64, 39) == 0);
        }
        sh_free(&m);
    }

    /* ---- the stripe report, at both resolutions -------------------------- */
    {
        ShReport off128, off512, on128, on512;
        CK(measure(128, 1.0f, 0.0f, &off128) == 0);
        CK(measure(512, 0.25f, 0.0f, &off512) == 0);
        CK(measure(128, 1.0f, SH_DEFAULT_BIAS, &on128) == 0);
        CK(measure(512, 0.25f, SH_DEFAULT_BIAS, &on512) == 0);

        printf("\n%-6s %-8s %8s %8s %8s %8s %8s %8s\n", "W", "bias",
               "stripes", "stripepx", "offsetpx", "neededpx", "errorpx", "ms");
        printf("%-6d %-8.1f %8d %8d %8.3f %8.3f %+8.3f %8.3f\n", 128, 0.0,
               off128.stripes, off128.stripe_pixels, off128.offset_px,
               off128.needed_px, off128.error_px, off128.ms);
        printf("%-6d %-8.1f %8d %8d %8.3f %8.3f %+8.3f %8.3f\n", 512, 0.0,
               off512.stripes, off512.stripe_pixels, off512.offset_px,
               off512.needed_px, off512.error_px, off512.ms);
        printf("%-6d %-8.1f %8d %8d %8.3f %8.3f %+8.3f %8.3f\n", 128,
               (double)SH_DEFAULT_BIAS, on128.stripes, on128.stripe_pixels,
               on128.offset_px, on128.needed_px, on128.error_px, on128.ms);
        printf("%-6d %-8.1f %8d %8d %8.3f %8.3f %+8.3f %8.3f\n", 512,
               (double)SH_DEFAULT_BIAS, on512.stripes, on512.stripe_pixels,
               on512.offset_px, on512.needed_px, on512.error_px, on512.ms);

        /* Stripes are visible without the offset, at both sizes. */
        CK(off128.stripes > 0 && off128.stripe_pixels > 0);
        CK(off512.stripes > 0 && off512.stripe_pixels > 0);
        /* The same world gives the same numbers at both sizes. */
        CK(off128.stripes == off512.stripes);
        CK(off128.stripe_pixels == off512.stripe_pixels);
        CK(on128.stripes == on512.stripes);
        CK(on128.stripe_pixels == on512.stripe_pixels);
        CK(fabsf(off128.error_px - off512.error_px) < 1e-3f);
        CK(fabsf(on128.error_px - on512.error_px) < 1e-3f);
        /* The default clears them. */
        CK(on128.stripes == 0);
        CK(on512.stripes == 0);
        /* The configured offset is at least what the surface needs. */
        CK(on128.offset_px >= on128.needed_px || on128.stripes == 0);
        CK(on128.ms > 0.0 && on512.ms > 0.0);
    }

    /* ---- offset off is the current picture, bit for bit ------------------ */
    {
        ShLight light = {64.0f, 10.0f, 40.0f, 1.0f};
        ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f},
                         {70.0f, 70.0f, 110.0f, 70.0f}};
        float old_buf[128 * 128];
        CK(sh_init(&m, 128, 128) == 0);
        CK(sh_set_bias(&m, 0.0f, SH_DEFAULT_SLOPE_CLAMP, &err) == 0);
        CK(sh_render(&m, &light, segs, 2) == 0);
        old_render(old_buf, 128, 128, &light, segs, 2);
        CK(memcmp(old_buf, m.buf, sizeof old_buf) == 0);
        sh_free(&m);
    }

    /* ---- bad input names the field -------------------------------------- */
    {
        ShSeg segs[3] = {{20.0f, 30.0f, 110.0f, 50.0f},
                         {60.0f, 20.0f, 60.0f, 110.0f},   /* vertical */
                         {20.0f, 30.0f, 110.0f, NAN}};    /* bad endpoint */
        ShLight light = {65.0f, 2.0f, 60.0f, 1.0f};
        float slope = 0.0f;
        float before[4 * 4];

        CK(sh_init(&m, 4, 4) == 0);
        CK(sh_set_bias(&m, -1.0f, 1.0f, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "bias") == 0);
        CK(sh_set_bias(&m, NAN, 1.0f, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "bias") == 0);
        CK(sh_set_bias(&m, INFINITY, 1.0f, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "bias") == 0);
        CK(sh_set_bias(&m, 1.0f, 0.0f, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "slope_clamp") == 0);
        CK(sh_set_bias(&m, 1.0f, NAN, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "slope_clamp") == 0);
        CK(sh_set_bias(&m, 1.0f, 1.0f, &err) == 0);
        CK(err.field[0] == '\0');

        CK(sh_seg_slope(segs, 1, &slope, &err) == SH_ERR_FIELD);
        CK(strcmp(err.field, "segs[1].slope") == 0);
        CK(sh_seg_slope(segs, 2, &slope, &err) == SH_ERR_FIELD);
        CK(strcmp(err.field, "segs[2].y1") == 0);
        /* A finite pair whose slope overflows float is still non-finite. */
        {
            ShSeg tiny = {0.0f, 0.0f, 1e-38f, 1e38f};
            CK(sh_seg_slope(&tiny, 0, &slope, &err) == SH_ERR_FIELD);
            CK(strcmp(err.field, "segs[0].slope") == 0);
        }
        /* The map is not modified when a segment is rejected. */
        memcpy(before, m.buf, sizeof before);
        CK(sh_render_ex(&m, &light, segs, 2, &err) == SH_ERR_FIELD);
        CK(strcmp(err.field, "segs[1].slope") == 0);
        CK(memcmp(before, m.buf, sizeof before) == 0);
        CK(sh_pixel_blocked(&m, &light, segs, 2, 1, 1) == SH_ERR_FIELD);
        sh_free(&m);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
