#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int same_map(const ShadowMap *a, const ShadowMap *b) {
    if (a->w != b->w || a->h != b->h) return 0;
    return memcmp(a->buf, b->buf, sizeof(float) * (size_t)a->w * (size_t)a->h) == 0;
}

int main(void) {
    ShadowMap m, ref;
    CK(sh_init(&m, 64, 64) == 0);
    CK(sh_init(&ref, 64, 64) == 0);
    ShLight l = {32.0f, 8.0f, 20.0f, 1.0f};
    ShSeg s = {10.0f, 20.0f, 54.0f, 20.0f};
    CK(sh_render(&m, &l, &s, 1) == 0);
    CK(m.buf[(size_t)32 * 64 + 32] < 0.01f);
    CK(m.buf[(size_t)2 * 64 + 32] > 0.5f);
    ShLight bad = {0.0f, 0.0f, 0.0f, 1.0f};
    CK(sh_render(&m, &bad, &s, 1) == SH_E_LIGHT);

    /* --- error reporting: zero segments ------------------------------- */
    ShError err;
    memset(&err, 0, sizeof err);
    CK(sh_render_ex(&m, &l, &s, 0, NULL, NULL, &err) == SH_E_NO_SEGMENTS);
    CK(err.code == SH_E_NO_SEGMENTS && err.index == -1 && err.msg != NULL);

    /* --- error reporting: non-finite segment, index 3 ----------------- */
    ShSeg segs[6] = {
        {10.0f, 20.0f, 54.0f, 20.0f},
        {12.0f, 30.0f, 50.0f, 30.0f},
        {14.0f, 40.0f, 40.0f, 40.0f},
        {NAN, 44.0f, 40.0f, 44.0f},
        {16.0f, 50.0f, 30.0f, 50.0f},
        {INFINITY, 8.0f, 20.0f, 8.0f}
    };
    ShCullOpts opts;
    memset(&opts, 0, sizeof opts);
    CK(sh_frustum_from_light(&m, &l, &opts.frustum, &err) == SH_OK);
    opts.enabled = 1;
    CK(sh_render_ex(&m, &l, segs, 6, &opts, NULL, &err) == SH_E_NONFINITE);
    CK(err.index == 3);
    /* Disabled path reports the same index. */
    opts.enabled = 0;
    CK(sh_render_ex(&m, &l, segs, 6, &opts, NULL, &err) == SH_E_NONFINITE);
    CK(err.index == 3);
    /* Direct culling call reports it too. */
    {
        int cand[6];
        ShCullStats st;
        CK(sh_cull_segments(&opts.frustum, segs, 6, cand, 6, &st, &err) == SH_E_NONFINITE);
        CK(err.index == 3);
    }
    /* A later non-finite segment is still reported (validation precedes the
     * capacity check for earlier segments only when there is room). */
    segs[5] = (ShSeg){20.0f, 8.0f, 20.0f, 8.0f};
    {
        int cand[6];
        CK(sh_cull_segments(&opts.frustum, segs, 6, cand, 6, NULL, &err)
           == SH_E_NONFINITE);
        CK(err.index == 3);
        /* With a zero-capacity table the first kept segment overflows first. */
        CK(sh_cull_segments(&opts.frustum, segs, 0, NULL, 0, NULL, &err)
           == SH_E_NO_SEGMENTS);
        CK(err.index == -1);
    }

    /* --- error reporting: empty / non-finite / non-covering frustum --- */
    {
        ShFrustum empty;
        memset(&empty, 0, sizeof empty);       /* n == 0 */
        ShCullOpts o = {1, empty};
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_EMPTY_FRUSTUM);
        CK(err.index == -1);

        ShFrustum sliver = {3, {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}}};
        o.frustum = sliver;
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_EMPTY_FRUSTUM);
        CK(err.index == -1);

        ShFrustum nf;
        CK(sh_frustum_from_light(&m, &l, &nf, NULL) == SH_OK);
        nf.v[2].x = NAN;
        o.frustum = nf;
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_NONFINITE);
        CK(err.index == 2);

        /* Too small frustum: the light itself falls outside.  bad == -1
         * denotes the light in the cover check. */
        ShFrustum tiny = {4, {{0.0f, 0.0f}, {16.0f, 0.0f}, {16.0f, 16.0f}, {0.0f, 16.0f}}};
        o.frustum = tiny;
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_FRUSTUM_COVER);
        CK(err.index == -1);

        /* Frustum holds the light and corners 0/1 but not corner 2. */
        ShFrustum tri = {3, {{32.0f, 8.0f}, {0.0f, 0.0f}, {64.0f, 0.0f}}};
        o.frustum = tri;
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_FRUSTUM_COVER);
        CK(err.index == 2);

        /* Concave polygon: rejected, and the reflex vertex is named. */
        ShFrustum dent = {4, {{0.0f, 0.0f}, {64.0f, 0.0f}, {10.0f, 10.0f}, {0.0f, 64.0f}}};
        o.frustum = dent;
        CK(sh_render_ex(&m, &l, &s, 1, &o, NULL, &err) == SH_E_EMPTY_FRUSTUM);
        CK(err.index == 2);
    }

    /* --- culling loss-free vs brute force ---------------------------- */
    /* Half the segments sit far below the light and outside its frustum, the
     * rest lie inside the map and must survive culling. */
    ShSeg many[64];
    for (int i = 0; i < 64; ++i) {
        static const ShSeg pat[4] = {
            {10.0f, 20.0f, 54.0f, 20.0f}, {5.0f, 56.0f, 60.0f, 60.0f},
            {0.0f, 12.0f, 8.0f, 46.0f},   {-40.0f, -60.0f, -20.0f, -30.0f}
        };
        many[i] = pat[i % 4];
    }
    ShCullOpts on;
    memset(&on, 0, sizeof on);
    on.enabled = 1;
    CK(sh_frustum_from_light(&m, &l, &on.frustum, NULL) == SH_OK);
    ShCullStats st;
    CK(sh_render_ex(&m, &l, many, 64, &on, &st, &err) == SH_OK);
    CK(st.input == 64);
    CK(st.candidates + st.culled == st.input);
    CK(st.candidates > 0 && st.culled > 0);

    ShCullOpts off = on;
    off.enabled = 0;
    CK(sh_render_ex(&ref, &l, many, 64, &off, NULL, NULL) == SH_OK);
    CK(same_map(&m, &ref));

    /* Candidate table is ascending and in original order. */
    {
        int cand[64];
        ShCullStats st2;
        CK(sh_cull_segments(&on.frustum, many, 64, cand, 64, &st2, NULL) == SH_OK);
        CK(st2.candidates == st.candidates && st2.culled == st.culled);
        for (int i = 1; i < st2.candidates; ++i) CK(cand[i - 1] < cand[i]);
        for (int i = 0; i < st2.candidates; ++i)
            CK(cand[i] >= 0 && cand[i] < 64);
        /* Capacity error names the overflowing segment. */
        ShCullStats st3;
        CK(sh_cull_segments(&on.frustum, many, 64, cand, st2.candidates - 1,
                            &st3, &err) == SH_E_CAPACITY);
        CK(err.index >= 0 && err.index < 64);
    }

    /* Layer-2-only render must equal the two-layer render. */
    {
        int cand[64];
        ShCullStats st2;
        ShadowMap m2;
        CK(sh_init(&m2, 64, 64) == 0);
        CK(sh_cull_segments(&on.frustum, many, 64, cand, 64, &st2, NULL) == SH_OK);
        CK(sh_render_candidates(&m2, &l, many, cand, st2.candidates) == SH_OK);
        CK(same_map(&m2, &ref));
        CK(sh_render_candidates(&m2, &l, many, cand, -1) == SH_E_ARG);
        sh_free(&m2);
    }

    /* Disabled path ignores the frustum entirely: an empty/malformed frustum
     * must not matter, and the result equals the brute-force map. */
    {
        ShadowMap m3;
        CK(sh_init(&m3, 64, 64) == 0);
        ShCullOpts broken; memset(&broken, 0, sizeof broken);
        CK(sh_render_ex(&m3, &l, many, 64, &broken, NULL, NULL) == SH_OK);
        CK(same_map(&m3, &ref));
        sh_free(&m3);
    }

    /* Zero-area segment is kept by the frustum layer if a point lies inside. */
    {
        int cand[1];
        ShCullStats st4;
        ShSeg dot = {32.0f, 32.0f, 32.0f, 32.0f};
        CK(sh_cull_segments(&on.frustum, &dot, 1, cand, 1, &st4, NULL) == SH_OK);
        CK(st4.candidates == 1);
        ShSeg far = {-50.0f, -50.0f, -40.0f, -40.0f};
        CK(sh_cull_segments(&on.frustum, &far, 1, cand, 1, &st4, NULL) == SH_OK);
        CK(st4.candidates == 0 && st4.culled == 1);
    }

    sh_free(&m);
    sh_free(&ref);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
