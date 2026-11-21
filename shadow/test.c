#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define W 64
#define H 64

/* Independent per-pixel reference: sum the contributions of the lights in an
 * order the test computes itself, using the documented single-light formula.
 * It shares no code with the library. */
static float ref_pixel(int x, int y, const ShLight *L, int n,
                       const ShSeg *segs, int nseg, const int *order) {
    float px = (float)x + 0.5f, py = (float)y + 0.5f, v = 0.0f;
    for (int k = 0; k < n; ++k) {
        const ShLight *l = &L[order[k]];
        int shadowed = 0;
        for (int i = 0; i < nseg && !shadowed; ++i) {
            const ShSeg *s = &segs[i];
            float rx = px - l->x, ry = py - l->y;
            float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
            float den = rx * sy - ry * sx;
            if (fabsf(den) < 1e-9f) continue;
            float t = ((s->x0 - l->x) * sy - (s->y0 - l->y) * sx) / den;
            float u = ((s->x0 - l->x) * ry - (s->y0 - l->y) * rx) / den;
            if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f) shadowed = 1;
        }
        if (!shadowed) {
            float dx = px - l->x, dy = py - l->y;
            v += l->intensity / (1.0f + sqrtf(dx * dx + dy * dy) / l->radius);
        }
    }
    return v;
}

static int same_bytes(const ShadowMap *a, const ShadowMap *b) {
    return a->w == b->w && a->h == b->h &&
           memcmp(a->buf, b->buf, (size_t)a->w * (size_t)a->h * sizeof(float)) == 0;
}

/* The fixture's lights have distinct positions, so the reference order can be
 * written down literally: intensity desc, tie on 0.60 broken by x ascending. */
static const int REF_ORDER[4] = {1, 3, 2, 0};

int main(void) {
    /* ---- legacy behaviour, unchanged ---------------------------------- */
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

    /* ---- multi-light scene -------------------------------------------- */
    const ShSeg segs[2] = {{20.0f, 40.0f, 44.0f, 40.0f},
                           {30.0f, 10.0f, 38.0f, 56.0f}};
    /* Deliberately unsorted input: intensities 0.35, 1.00, 0.60, 0.60.
     * The last two also tie on intensity and are separated by x. */
    const ShLight L[4] = {
        {50.0f, 50.0f, 22.0f, 0.35f},
        {10.0f, 12.0f, 18.0f, 1.00f},
        {56.0f, 20.0f, 30.0f, 0.60f},
        {22.0f, 52.0f, 26.0f, 0.60f},
    };
    const int expected_order[4] = {1, 3, 2, 0}; /* 1.00, .60@x=22, .60@x=56, .35 */

    /* ---- order: brightest first, ties by x --------------------------- */
    ShLightOrder ord[4];
    ShError err;
    CK(sh_lights_order(L, 4, ord, &err) == SH_OK);
    for (int i = 0; i < 4; ++i) CK(ord[i].index == expected_order[i]);
    CK(ord[0].intensity == 1.00f && ord[3].intensity == 0.35f);
    /* every light appears exactly once */
    {
        int seen[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) seen[ord[i].index]++;
        CK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1);
    }

    /* ---- permutation invariance, bit for bit -------------------------- */
    ShadowMap base;
    CK(sh_init(&base, W, H) == 0);
    ShMultiStats st;
    CK(sh_render_lights(&base, L, 4, segs, 2, 1, &st, &err) == SH_OK);
    CK(st.lights == 4 && st.passes == 4 && st.sorted == 1);
    CK(st.pixels == (unsigned long long)W * H);
    CK(st.accumulate_ops == (unsigned long long)W * H * 4);
    CK(st.temp_maps == 0);
    CK(st.map_bytes == (size_t)W * H * sizeof(float));
    CK(st.order_bytes == 4 * sizeof(ShLightOrder));
    CK(st.peak_extra_bytes == st.order_bytes);
    CK(st.avoided_temp_bytes == st.map_bytes * 4);
    CK(st.total_bytes == st.map_bytes + st.order_bytes);
    for (int i = 0; i < 4; ++i) CK(st.order[i].index == expected_order[i]);

    {
        /* all 24 permutations, including reversing the tie pair */
        int perm[4], idx[4] = {0, 1, 2, 3};
        for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
        for (int c = 0; c < 4; ++c) for (int d = 0; d < 4; ++d) {
            if (b == a || c == a || d == a || c == b || d == b || d == c) continue;
            perm[0] = a; perm[1] = b; perm[2] = c; perm[3] = d;
            ShLight shuffled[4];
            for (int i = 0; i < 4; ++i) shuffled[i] = L[idx[perm[i]]];
            ShadowMap mm;
            if (sh_init(&mm, W, H) != 0) { CK(0); continue; }
            ShMultiStats s2;
            int rc = sh_render_lights(&mm, shuffled, 4, segs, 2, 1, &s2, &err);
            CK(rc == SH_OK);
            CK(same_bytes(&mm, &base));
            CK(s2.accumulate_ops == st.accumulate_ops);
            /* stats index the shuffled array; map back and require the same
             * lights in the same ranks as the unshuffled run */
            for (int i = 0; i < 4; ++i)
                CK(perm[s2.order[i].index] == REF_ORDER[i]);
            sh_free(&mm);
        }
    }

    /* equal-valued lights: swapping them is a no-op too */
    {
        ShLight dup[2] = {{20.0f, 30.0f, 15.0f, 0.5f}, {20.0f, 30.0f, 15.0f, 0.5f}};
        ShadowMap a, b;
        CK(sh_init(&a, W, H) == 0 && sh_init(&b, W, H) == 0);
        CK(sh_render_lights(&a, dup, 2, segs, 2, 1, NULL, &err) == SH_OK);
        ShLight swap[2] = {dup[1], dup[0]};
        CK(sh_render_lights(&b, swap, 2, segs, 2, 1, NULL, &err) == SH_OK);
        CK(same_bytes(&a, &b));
        sh_free(&a); sh_free(&b);
    }

    /* ---- single light through the multi path == legacy, bit for bit --- */
    {
        ShadowMap a, b;
        CK(sh_init(&a, W, H) == 0 && sh_init(&b, W, H) == 0);
        CK(sh_render(&a, &L[1], segs, 2) == 0);
        CK(sh_render_lights(&b, &L[1], 1, segs, 2, 1, NULL, &err) == SH_OK);
        CK(same_bytes(&a, &b));
        sh_free(&a); sh_free(&b);
    }

    /* ---- enable_multi == 0 reproduces sh_render(lights[0]) ------------- */
    {
        ShadowMap a, b;
        CK(sh_init(&a, W, H) == 0 && sh_init(&b, W, H) == 0);
        CK(sh_render(&a, &L[0], segs, 2) == 0);
        ShMultiStats s0;
        CK(sh_render_lights(&b, L, 4, segs, 2, 0, &s0, &err) == SH_OK);
        CK(same_bytes(&a, &b));
        CK(s0.lights == 1 && s0.passes == 1);
        CK(s0.accumulate_ops == (unsigned long long)W * H);
        CK(s0.temp_maps == 0);
        CK(s0.order[0].index == 0);
        sh_free(&a); sh_free(&b);
    }

    /* ---- per-pixel reference, in the sorted order --------------------- */
    {
        const int *order = REF_ORDER;
        double max_err = 0.0, sum_err = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float want = ref_pixel(x, y, L, 4, segs, 2, order);
                float got = base.buf[(size_t)y * W + x];
                double e = fabs((double)got - (double)want);
                if (e > max_err) max_err = e;
                sum_err += e;
                if (e != 0.0) CK(0);
            }
        CK(max_err == 0.0);
        (void)sum_err;
        /* and the result is genuinely brighter than any single light */
        float single_max = 0.0f;
        for (int i = 0; i < W * H; ++i) if (base.buf[i] > single_max) single_max = base.buf[i];
        CK(single_max > 1.0f); /* several unoccluded lights overlap somewhere */
    }

    /* ---- rejecting bad values names the field ------------------------- */
    {
        ShLight bad_light[2] = {L[0], L[1]};
        bad_light[1].radius = 0.0f;
        ShadowMap mm; CK(sh_init(&mm, 16, 16) == 0);
        memset(&err, 0, sizeof err);
        CK(sh_render_lights(&mm, bad_light, 2, segs, 2, 1, NULL, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[1].radius") == 0);
        CK(err.light == 1);
        CK(strstr(err.msg, "must be > 0") != NULL);

        bad_light[1] = L[1]; bad_light[0].intensity = -1.0f;
        CK(sh_render_lights(&mm, bad_light, 2, segs, 2, 1, NULL, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[0].intensity") == 0);

        bad_light[0] = L[0]; bad_light[0].x = NAN;
        CK(sh_render_lights(&mm, bad_light, 2, segs, 2, 1, NULL, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[0].x") == 0);

        bad_light[0] = L[0]; bad_light[1].intensity = INFINITY;
        CK(sh_render_lights(&mm, bad_light, 2, segs, 2, 1, NULL, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[1].intensity") == 0);

        /* the same checks apply when multi is off */
        bad_light[1] = L[1]; bad_light[0].y = NAN;
        CK(sh_render_lights(&mm, bad_light, 2, segs, 2, 0, NULL, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[0].y") == 0);

        /* bad segments */
        ShSeg bad_segs[2] = {segs[0], segs[1]};
        bad_segs[1].x1 = NAN;
        CK(sh_render_lights(&mm, L, 4, bad_segs, 2, 1, NULL, &err) == SH_ERR_SEG);
        CK(strcmp(err.field, "segs[1].x1") == 0);

        /* shape of the arguments */
        CK(sh_render_lights(&mm, L, 0, segs, 2, 1, NULL, &err) == SH_ERR_NOLIGHT);
        CK(strcmp(err.field, "nlight") == 0);
        CK(sh_render_lights(&mm, NULL, 4, segs, 2, 1, NULL, &err) == SH_ERR_NULL);
        CK(strcmp(err.field, "lights") == 0);
        CK(sh_render_lights(NULL, L, 4, segs, 2, 1, NULL, &err) == SH_ERR_NULL);
        CK(strcmp(err.field, "m") == 0);
        CK(sh_render_lights(&mm, L, 4, NULL, 2, 1, NULL, &err) == SH_ERR_NULL);
        CK(strcmp(err.field, "segs") == 0);
        CK(sh_render_lights(&mm, L, 4, segs, -1, 1, NULL, &err) == SH_ERR_ARG);
        CK(strcmp(err.field, "nseg") == 0);

        /* a rejected call leaves the map untouched */
        ShadowMap keep; CK(sh_init(&keep, 16, 16) == 0);
        for (int i = 0; i < 16 * 16; ++i) keep.buf[i] = 0.25f;
        bad_light[0] = L[0]; bad_light[1] = L[1]; bad_light[1].radius = -2.0f;
        CK(sh_render_lights(&keep, bad_light, 2, segs, 2, 1, NULL, &err) == SH_ERR_LIGHT);
        for (int i = 0; i < 16 * 16; ++i) CK(keep.buf[i] == 0.25f);
        sh_free(&keep);
        sh_free(&mm);
    }

    /* ---- stage 1 on its own rejects too ------------------------------- */
    {
        ShLight one[1] = {{0.0f, 0.0f, 1.0f, 1.0f}};
        one[0].radius = -1.0f;
        CK(sh_lights_order(one, 1, ord, &err) == SH_ERR_LIGHT);
        CK(strcmp(err.field, "lights[0].radius") == 0);
        CK(sh_lights_order(L, 4, NULL, &err) == SH_ERR_NULL);
        CK(strcmp(err.field, "order") == 0);
    }

    /* ---- empty scene: pure falloff, order irrelevant ------------------ */
    {
        ShadowMap a, b;
        CK(sh_init(&a, W, H) == 0 && sh_init(&b, W, H) == 0);
        CK(sh_render_lights(&a, L, 4, NULL, 0, 1, NULL, &err) == SH_OK);
        const ShLight rev[4] = {L[3], L[2], L[1], L[0]};
        CK(sh_render_lights(&b, rev, 4, NULL, 0, 1, NULL, &err) == SH_OK);
        CK(same_bytes(&a, &b));
        sh_free(&a); sh_free(&b);
    }

    /* ---- order table export ------------------------------------------ */
    {
        CK(sh_write_order_table("sh_order_test.csv", st.order, st.lights,
                                "tie-fixture", W, H) == SH_OK);
        FILE *f = fopen("sh_order_test.csv", "rb");
        CK(f != NULL);
        if (f) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = '\0';
            fclose(f);
            CK(strstr(buf, "rank,index,intensity,x,y,radius") != NULL);
            CK(strstr(buf, "# scene=tie-fixture w=64 h=64 passes=4") != NULL);
            /* rank 0 must be input index 1 (intensity 1.00) */
            /* skip the two header lines, then the rank-0 row */
            const char *line = strchr(buf, '\n');
            line = line ? strchr(line + 1, '\n') : NULL;
            line = line ? line + 1 : NULL;
            CK(line && strncmp(line, "0,1,1,", 6) == 0);
        }
        CK(sh_write_order_table(NULL, st.order, 4, "x", W, H) == SH_ERR_NULL);
        CK(sh_write_order_table("sh_order_test.csv", NULL, 4, "x", W, H) == SH_ERR_NULL);
        CK(sh_write_order_table("sh_order_test.csv", st.order, -1, "x", W, H) == SH_ERR_ARG);
        remove("sh_order_test.csv");
    }

    sh_free(&base);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
