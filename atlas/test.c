#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Verbatim copy of the original sequential shelf packer, used as the
 * byte-for-byte compatibility oracle. */
static int ref_pack(const AtlasRect *rects, int n, int width,
                    AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0) return -1;
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) return -2;
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    return 0;
}

/* Geometric validator: inside the strip, no overlap, area matches stats. */
static int validate(const AtlasRect *rects, int n, int width,
                    const AtlasPos *p, const AtlasStats *s) {
    int area = 0;
    for (int i = 0; i < n; ++i) {
        int w = p[i].rotated ? rects[i].h : rects[i].w;
        int h = p[i].rotated ? rects[i].w : rects[i].h;
        if (!p[i].placed) return 1;
        if (p[i].x < 0 || p[i].y < 0 || p[i].x + w > width) return 2;
        area += rects[i].w * rects[i].h;
        for (int j = 0; j < i; ++j) {
            int wj = p[j].rotated ? rects[j].h : rects[j].w;
            int hj = p[j].rotated ? rects[j].w : rects[j].h;
            int ox = p[i].x < p[j].x + wj && p[j].x < p[i].x + w;
            int oy = p[i].y < p[j].y + hj && p[j].y < p[i].y + h;
            if (ox && oy) return 3;
        }
    }
    if (area != s->used_px) return 4;
    if (s->height <= 0) return 5;
    return 0;
}

int main(void) {
    /* ---- legacy API still behaves exactly as before ---- */
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4], q[4];
    AtlasStats s, t;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0 && p[0].rotated == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    CK(ref_pack(r, 4, 64, q, &t) == 0);
    for (int i = 0; i < 4; ++i) {
        CK(p[i].x == q[i].x && p[i].y == q[i].y && p[i].placed == q[i].placed);
    }
    CK(s.shelves == t.shelves && s.used_px == t.used_px);

    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);

    /* ---- flags=0 + identity order is byte-identical to the original ---- */
    int id4[4] = {0, 1, 2, 3};
    int err = 0;
    CK(atlas_pack_ex(r, 4, 64, 0, id4, p, &s, &err) == 0 && err == -1);
    for (int i = 0; i < 4; ++i) {
        CK(p[i].x == q[i].x && p[i].y == q[i].y && p[i].rotated == 0);
    }

    /* larger random-ish case, all four configurations stay geometrically valid */
    enum { N = 40 };
    AtlasRect big[N];
    unsigned seed = 12345u;
    for (int i = 0; i < N; ++i) {
        seed = seed * 1103515245u + 12345u;
        big[i].w = 4 + (seed >> 8) % 60;
        seed = seed * 1103515245u + 12345u;
        big[i].h = 8 + (seed >> 8) % 120;
    }
    AtlasPos pp[N], qq[N];
    AtlasStats ss;
    unsigned modes[4] = {0, ATLAS_SORT_HEIGHT, ATLAS_ALLOW_ROTATE,
                         ATLAS_SORT_HEIGHT | ATLAS_ALLOW_ROTATE};
    for (int m = 0; m < 4; ++m) {
        CK(atlas_pack_ex(big, N, 128, modes[m], NULL, pp, &ss, NULL) == 0);
        CK(validate(big, N, 128, pp, &ss) == 0);
    }
    /* baseline mode must still match the reference exactly */
    CK(atlas_pack_ex(big, N, 128, 0, NULL, pp, &ss, NULL) == 0);
    CK(validate(big, N, 128, pp, &ss) == 0);
    CK(ref_pack(big, N, 128, qq, &t) == 0);
    for (int i = 0; i < N; ++i) CK(pp[i].x == qq[i].x && pp[i].y == qq[i].y);

    /* ---- stage 1: height sort puts the tallest rect first ---- */
    AtlasRect h3[3] = {{10, 10}, {10, 40}, {10, 20}};
    AtlasPos hp[3];
    AtlasStats hs;
    CK(atlas_pack_ex(h3, 3, 20, ATLAS_SORT_HEIGHT, NULL, hp, &hs, NULL) == 0);
    CK(hp[1].y == 0);                 /* the 40-tall rect leads the first shelf */
    CK(hp[2].y == 0);                 /* the 20-tall rect shares it             */
    CK(hp[0].y >= 40);                /* the 10-tall rect lands on shelf two    */

    /* custom binning order is honored */
    int ord3[3] = {2, 0, 1};
    CK(atlas_pack_ex(h3, 3, 64, 0, ord3, hp, &hs, NULL) == 0);
    CK(hp[2].x == 0 && hp[2].y == 0);

    /* ---- stage 2: rotation rescues a rect that otherwise opens a shelf ---- */
    AtlasRect rot2[2] = {{40, 12}, {48, 10}};
    AtlasPos rp[2];
    AtlasStats rs;
    CK(ref_pack(rot2, 2, 64, q, &t) == 0 && t.shelves == 2);
    CK(atlas_pack_ex(rot2, 2, 64, ATLAS_ALLOW_ROTATE, NULL, rp, &rs, NULL) == 0);
    CK(rs.shelves == 1);
    CK(rp[0].rotated == 0 && rp[1].rotated == 1);
    CK(rp[1].x == 40);                                   /* 40 + rotated width 10 */
    CK(validate(rot2, 2, 64, rp, &rs) == 0);

    /* a rect fitting flat on the current shelf is never rotated */
    AtlasRect flat[2] = {{20, 8}, {10, 30}};
    CK(atlas_pack_ex(flat, 2, 64, ATLAS_ALLOW_ROTATE, NULL, rp, &rs, NULL) == 0);
    CK(rp[0].rotated == 0 && rp[1].rotated == 0);

    /* ---- rejection: zero / negative dimensions, with offending index ---- */
    AtlasRect zr[3] = {{10, 10}, {0, 8}, {6, 6}};
    err = -9;
    CK(atlas_pack_ex(zr, 3, 64, 0, NULL, p, &s, &err) == -2 && err == 1);
    AtlasRect nr[3] = {{10, 10}, {8, -4}, {6, 6}};
    CK(atlas_pack_ex(nr, 3, 64, 0, NULL, p, &s, &err) == -2 && err == 1);

    /* ---- rejection: strip too narrow, even after rotation ---- */
    AtlasRect wide[2] = {{10, 10}, {80, 70}};
    CK(atlas_pack_ex(wide, 2, 64, 0, NULL, p, &s, &err) == -2 && err == 1);
    CK(atlas_pack_ex(wide, 2, 64, ATLAS_ALLOW_ROTATE, NULL, p, &s, &err) == -2
       && err == 1);                                   /* 70 and 80 both exceed 64 */
    AtlasRect rescue[1] = {{80, 40}};
    CK(atlas_pack_ex(rescue, 1, 64, ATLAS_ALLOW_ROTATE, NULL, p, &s, &err) == 0);
    CK(p[0].rotated == 1);

    /* ---- rejection: order out of range and duplicates, with sequence position ---- */
    int bad_ord[3] = {0, 3, 1};
    CK(atlas_pack_ex(r, 3, 64, 0, bad_ord, p, &s, &err) == -3 && err == 1);
    int neg_ord[3] = {-1, 0, 1};
    CK(atlas_pack_ex(r, 3, 64, 0, neg_ord, p, &s, &err) == -3 && err == 0);
    int dup_ord[4] = {0, 2, 2, 1};
    CK(atlas_pack_ex(r, 4, 64, 0, dup_ord, p, &s, &err) == -3 && err == 2);

    /* bad arguments */
    CK(atlas_pack_ex(NULL, 4, 64, 0, NULL, p, &s, NULL) == -1);
    CK(atlas_pack_ex(r, 4, 0, 0, NULL, p, &s, NULL) == -1);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
