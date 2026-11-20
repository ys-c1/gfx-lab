#include "atlas.h"
#include <stdio.h>

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Independently check the layout: every placed rect inside the strip, no two
 * overlapping, and each output at its point carries the source dimensions
 * (swapped when rotated 90 degrees). */
static int layout_ok(const AtlasRect *r, const AtlasPos *p, int n, int width,
                     int height) {
    for (int i = 0; i < n; ++i) {
        if (!p[i].placed || (p[i].rot != 0 && p[i].rot != 90)) return 0;
        int w = p[i].rot == 90 ? r[i].h : r[i].w;
        int h = p[i].rot == 90 ? r[i].w : r[i].h;
        if (p[i].x < 0 || p[i].y < 0 || p[i].x + w > width) return 0;
        if (height > 0 && p[i].y + h > height) return 0;
        for (int j = 0; j < i; ++j) {
            int jw = p[j].rot == 90 ? r[j].h : r[j].w;
            int jh = p[j].rot == 90 ? r[j].w : r[j].h;
            if (p[i].x < p[j].x + jw && p[j].x < p[i].x + w &&
                p[i].y < p[j].y + jh && p[j].y < p[i].y + h) return 0;
        }
    }
    return 1;
}

/* Legacy layout must be byte-for-byte what the original shelf code produced. */
static void test_legacy_layout(void) {
    AtlasRect r[4] = {{ 16, 16 }, { 32, 8 }, { 8, 8 }, { 64, 4 }};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16 * 16 + 32 * 8 + 8 * 8 + 64 * 4);
    AtlasRect bad[1] = {{ 128, 4 }};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);
    /* Reference positions from the original sequential shelf packer. */
    AtlasRect ref[5] = {{ 20, 10 }, { 30, 12 }, { 14, 6 }, { 20, 20 }, { 18, 8 }};
    AtlasPos rp[5];
    CK(atlas_pack(ref, 5, 64, rp, &s) == 0);
    CK(rp[0].x == 0  && rp[0].y == 0);
    CK(rp[1].x == 20 && rp[1].y == 0);
    CK(rp[2].x == 50 && rp[2].y == 0);
    CK(rp[3].x == 0  && rp[3].y == 12);   /* wrapped: shelf height was 12 */
    CK(rp[4].x == 20 && rp[4].y == 12);
    CK(s.shelves == 2 && s.height == 32);
}

static void test_maxrect_errors(void) {
    AtlasRect r[3] = {{ 16, 16 }, { 32, 8 }, { 8, 8 }};
    AtlasPos p[3];
    AtlasStats s;
    AtlasError e;

    AtlasRect big[2] = {{ 16, 16 }, { 200, 200 }};
    CK(atlas_pack_maxrect(big, 2, 128, p, &s, &e) == ATLAS_ERR_TOO_BIG);
    CK(e.index == 1);
    /* A rect that does not fit at 0 degrees but does after rotation is fine. */
    AtlasRect tall[1] = {{ 180, 10 }};
    CK(atlas_pack_maxrect(tall, 1, 100, p, &s, &e) == 0);
    CK(p[0].rot == 90 && p[0].x == 0 && p[0].y == 0);
    /* Unbounded strip: a tall rect fits at 0 degrees, no rotation needed. */
    AtlasRect upright[1] = {{ 100, 300 }};
    CK(atlas_pack_maxrect(upright, 1, 128, p, &s, &e) == 0);
    CK(p[0].rot == 0 && s.height == 300);

    AtlasRect zero[2] = {{ 16, 16 }, { 0, 8 }};
    CK(atlas_pack_maxrect(zero, 2, 128, p, &s, &e) == ATLAS_ERR_RECT);
    CK(e.index == 1);

    /* Bounded strip: 25x25 cannot fit a 20-tall strip in either orientation. */
    AtlasRect cube[2] = {{ 16, 16 }, { 25, 25 }};
    AtlasJob job = { cube, 2, 128, 20, ATLAS_MAXRECT, NULL };
    CK(atlas_pack_job(&job, p, &s, &e) == ATLAS_ERR_TOO_BIG);
    CK(e.index == 1);

    /* Free table emptied by an exact fill, with a rect still pending. */
    AtlasRect fill[3] = {{ 128, 10 }, { 128, 10 }, { 128, 10 }};
    AtlasPos p3[3];
    AtlasJob j2 = { fill, 3, 128, 20, ATLAS_MAXRECT, NULL };
    CK(atlas_pack_job(&j2, p3, &s, &e) == ATLAS_ERR_EMPTY);
    CK(e.index == 2);

    /* Free rects remain but none is large enough: the 128x12 goes first, so
     * the 128x10 (source index 0) is the one left over. */
    AtlasRect tight[2] = {{ 128, 10 }, { 128, 12 }};
    AtlasJob j2b = { tight, 2, 128, 20, ATLAS_MAXRECT, NULL };
    CK(atlas_pack_job(&j2b, p, &s, &e) == ATLAS_ERR_NO_FIT);
    CK(e.index == 0);

    /* Illegal orientation hint. */
    int hints[3] = { ATLAS_ROT_ANY, 45, ATLAS_ROT_0 };
    AtlasJob j3 = { r, 3, 128, 0, ATLAS_MAXRECT, hints };
    CK(atlas_pack_job(&j3, p, &s, &e) == ATLAS_ERR_ORIENT);
    CK(e.index == 1);

    /* Pinned orientation that cannot fit the strip. */
    int pin[2] = { ATLAS_ROT_0, ATLAS_ROT_90 };
    AtlasRect wide[2] = {{ 100, 20 }, { 10, 90 }};
    AtlasJob j4 = { wide, 2, 64, 0, ATLAS_MAXRECT, pin };
    CK(atlas_pack_job(&j4, p, &s, &e) == ATLAS_ERR_TOO_BIG);
    CK(e.index == 0);

    /* A pinned orientation that fits the strip but not the space left over is
     * an orientation error; the free rects exist but cannot honour the pin. */
    int pin2[2] = { ATLAS_ROT_0, ATLAS_ROT_0 };
    AtlasRect pinned[2] = {{ 100, 60 }, { 30, 70 }};
    AtlasJob j5 = { pinned, 2, 100, 100, ATLAS_MAXRECT, pin2 };
    CK(atlas_pack_job(&j5, p, &s, &e) == ATLAS_ERR_ORIENT);
    CK(e.index == 1);
    /* With both orientations allowed the second one rotates and fits. */
    AtlasJob j6 = { pinned, 2, 100, 100, ATLAS_MAXRECT, NULL };
    CK(atlas_pack_job(&j6, p, &s, &e) == 0);
    CK(p[1].rot == 90 && p[1].y == 60);
}

static void test_maxrect_layout(void) {
    AtlasRect r[3] = {{ 16, 16 }, { 32, 8 }, { 8, 8 }};
    AtlasPos p[3];
    AtlasStats s;
    /* 0 and 90 give the same area here; 0 degrees must win the tie. */
    CK(atlas_pack_maxrect(r, 3, 64, p, &s, NULL) == 0);
    CK(layout_ok(r, p, 3, 64, 0));
    CK(s.used_px == 16 * 16 + 32 * 8 + 8 * 8);

    /* A 180x10 item on a 100-wide strip must rotate; height is then 180 and
     * the unused 90px of every row is waste. */
    AtlasRect rot[1] = {{ 180, 10 }};
    CK(atlas_pack_maxrect(rot, 1, 100, p, &s, NULL) == 0);
    CK(p[0].rot == 90 && s.rotated == 1);
    CK(s.height == 180 && s.waste > 0.8999 && s.waste < 0.9001);

    /* Maxrect should not lose to the shelf layout on a mixed workload. */
    AtlasRect mixed[8];
    for (int i = 0; i < 8; ++i) { mixed[i].w = 8 + i * 4; mixed[i].h = 8 + (i % 3) * 8; }
    AtlasPos mp[8], sp[8];
    AtlasStats ms, ss;
    CK(atlas_pack_maxrect(mixed, 8, 128, mp, &ms, NULL) == 0);
    CK(atlas_pack(mixed, 8, 128, sp, &ss) == 0);
    CK(layout_ok(mixed, mp, 8, 128, 0));
    CK(ms.waste <= ss.waste + 1e-12);

    /* Index backfill: out[i] belongs to source rect i, not to visit order. */
    AtlasRect no[3] = {{ 5, 90 }, { 80, 5 }, { 40, 40 }};
    CK(atlas_pack_maxrect(no, 3, 64, p, &s, NULL) == 0);
    for (int i = 0; i < 3; ++i) {
        int w = p[i].rot == 90 ? no[i].h : no[i].w;
        int h = p[i].rot == 90 ? no[i].w : no[i].h;
        int ok = 0;
        if (p[i].x + w <= 64) {
            ok = 1;
            for (int j = 0; j < 3 && ok; ++j) {
                if (j == i) continue;
                int jw = p[j].rot == 90 ? no[j].h : no[j].w;
                int jh = p[j].rot == 90 ? no[j].w : no[j].h;
                if (p[i].x < p[j].x + jw && p[j].x < p[i].x + w &&
                    p[i].y < p[j].y + jh && p[j].y < p[i].y + h) ok = 0;
            }
        }
        CK(ok);
    }

    /* Largest item first: the 5x90 item anchors the bottom-left at 0,0. */
    CK(p[0].x == 0 && p[0].y == 0 && p[0].rot == 0);
}

static void test_bounded(void) {
    AtlasRect r[2] = {{ 40, 40 }, { 40, 40 }};
    AtlasPos p[2];
    AtlasStats s;
    AtlasError e;
    AtlasJob job = { r, 2, 80, 40, ATLAS_MAXRECT, NULL };
    CK(atlas_pack_job(&job, p, &s, &e) == 0);
    CK(s.height == 40 && s.waste == 0.0);
    CK(p[0].x == 0 && p[1].x == 40);

    /* Shelf mode refuses job fields that would change its layout. */
    AtlasJob j2 = { r, 2, 80, 0, ATLAS_SHELF, (const int[]){ ATLAS_ROT_0, ATLAS_ROT_0 } };
    CK(atlas_pack_job(&j2, p, &s, &e) == ATLAS_ERR_ARGS);
    AtlasJob j3 = { r, 2, 80, 40, ATLAS_SHELF, NULL };
    CK(atlas_pack_job(&j3, p, &s, &e) == ATLAS_ERR_ARGS);
    AtlasJob j4 = { r, 2, 80, 0, ATLAS_SHELF, NULL };
    CK(atlas_pack_job(&j4, p, &s, &e) == 0);
}

static void test_bounds(void) {
    AtlasRect r[4] = {{ 16, 16 }, { 32, 8 }, { 8, 8 }, { 64, 4 }};
    double lb = atlas_waste_lower_bound(r, 4, 64);
    double ub = atlas_waste_upper_bound(r, 4, 64);
    AtlasPos p[4];
    AtlasStats s;
    /* Area 256+256+64+256 = 832, largest short side 16: the best any packing
     * can do is height 16, i.e. 832/1024 = 81.25% used. */
    CK(atlas_pack_maxrect(r, 4, 64, p, &s, NULL) == 0);
    double expect = 1.0 - 832.0 / (64.0 * 16.0);
    CK(lb > expect - 1e-9 && lb < expect + 1e-9);
    CK(s.waste >= lb - 1e-12);
    CK(ub >= s.waste - 1e-12);
    CK(atlas_waste_lower_bound(r, 0, 64) < 0.0);
    CK(atlas_waste_upper_bound(r, 0, 64) < 0.0);
    /* A rect wider than the strip in both orientations has no bound. */
    AtlasRect nope[1] = {{ 100, 100 }};
    CK(atlas_waste_upper_bound(nope, 1, 64) < 0.0);
    CK(atlas_waste_lower_bound(nope, 1, 64) >= 0.0);
}

static void test_generated(void) {
    enum { N = 200 };
    AtlasRect r[N];
    AtlasPos p[N];
    AtlasStats s;
    AtlasError e;
    atlas_make_rects(r, N, 8, 64, 12345u);
    int area = 0;
    for (int i = 0; i < N; ++i) area += r[i].w * r[i].h;
    CK(atlas_pack_maxrect(r, N, 512, p, &s, &e) == 0);
    CK(layout_ok(r, p, N, 512, 0));
    CK(s.used_px == area);
    CK(s.waste >= atlas_waste_lower_bound(r, N, 512) - 1e-12);
    CK(s.waste <= atlas_waste_upper_bound(r, N, 512) + 1e-12);
    CK(s.free_rects >= 0);
}

int main(void) {
    test_legacy_layout();
    test_maxrect_errors();
    test_maxrect_layout();
    test_bounded();
    test_bounds();
    test_generated();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
