#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Opaque bbox is [x0,x1] x [y0,y1] inclusive; x1 < 0 makes a fully
 * transparent tile.  The rest is alpha 0. */
static unsigned char *mk_tile(int w, int h, int x0, int y0, int x1, int y1) {
    unsigned char *p = (unsigned char *)calloc((size_t)w * (size_t)h, 4);
    if (!p) return NULL;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            p[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] = 255;
    return p;
}

static int overlaps(const AtlasSlot *a, const AtlasSlot *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

int main(void) {
    /* ---- legacy path is unchanged -------------------------------- */
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    CK(s.saved_px == 0);
    CK(s.height == p[3].y + 4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);

    /* ---- stage 1: bounding box and margin ----------------------- */
    AtlasTile t1[1];
    AtlasTrim tr[1];
    int bi = 123;
    unsigned char *a = mk_tile(10, 8, 2, 1, 7, 5);  /* bbox 6x5 at (2,1) */
    CK(a != NULL);
    t1[0] = (AtlasTile){10, 8, a, 0};
    CK(atlas_trim(t1, 1, tr, &bi) == ATLAS_OK);
    CK(bi == -1);
    CK(tr[0].src_x == 2 && tr[0].src_y == 1);
    CK(tr[0].w == 6 && tr[0].h == 5);
    CK(tr[0].tile == 0);

    t1[0].margin = 1;
    CK(atlas_trim(t1, 1, tr, &bi) == ATLAS_OK);
    CK(tr[0].src_x == 1 && tr[0].src_y == 0);
    CK(tr[0].w == 8 && tr[0].h == 7);

    /* margin pushes the box past the tile edge -> error names tile 0 */
    t1[0].margin = 2;
    CK(atlas_trim(t1, 1, tr, &bi) == ATLAS_ETRIM_BOUNDS);
    CK(bi == 0);

    /* fully opaque tile cannot take any margin */
    unsigned char *full = mk_tile(4, 4, 0, 0, 3, 3);
    AtlasTile tf[1] = {{4, 4, full, 1}};
    CK(atlas_trim(tf, 1, tr, &bi) == ATLAS_ETRIM_BOUNDS);
    CK(bi == 0);
    tf[0].margin = 0;
    CK(atlas_trim(tf, 1, tr, &bi) == ATLAS_OK);
    CK(tr[0].w == 4 && tr[0].h == 4);

    /* negative margin -> error names tile 0 */
    t1[0].margin = -1;
    CK(atlas_trim(t1, 1, tr, &bi) == ATLAS_EMARGIN);
    CK(bi == 0);

    /* all-transparent tile -> error names the offending index (not 0) */
    unsigned char *empty = mk_tile(6, 6, 0, 0, -1, -1);
    unsigned char *ok = mk_tile(6, 6, 1, 1, 4, 4);
    AtlasTile t2[2] = {{6, 6, ok, 0}, {6, 6, empty, 0}};
    AtlasTrim tr2[2];
    CK(atlas_trim(t2, 2, tr2, &bi) == ATLAS_ETRANSPARENT);
    CK(bi == 1);
    t2[0].rgba = NULL;  /* NULL means fully opaque, no scan */
    CK(atlas_trim(t2, 1, tr2, &bi) == ATLAS_OK);
    CK(tr2[0].w == 6 && tr2[0].h == 6);
    CK(atlas_trim(NULL, 1, tr2, &bi) == ATLAS_EINVAL && bi == -1);

    /* ---- stage 2: write-back, no overlap, fit ------------------- */
    enum { N = 5 };
    AtlasTile ts[N];
    unsigned char *pix[N];
    int ow[N] = {10, 9, 12, 11, 8};
    int oh[N] = {8, 7, 9, 6, 10};
    for (int i = 0; i < N; ++i) {
        pix[i] = mk_tile(ow[i], oh[i], 1, 1, ow[i]-2, oh[i]-2);
        ts[i] = (AtlasTile){ow[i], oh[i], pix[i], 0};
    }
    AtlasSlot out[N];
    AtlasStats st;
    bi = 99;
    /* trimmed widths are ow-2, all fit width 32 */
    CK(atlas_build(ts, N, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_OK);
    CK(bi == -1);
    CK(st.used_px == (10-2)*(8-2) + (9-2)*(7-2) + (12-2)*(9-2) + (11-2)*(6-2) + (8-2)*(10-2));
    long long orig = 0, trimmed = 0;
    for (int i = 0; i < N; ++i) {
        orig += (long long)ow[i] * oh[i];
        trimmed += (long long)out[i].w * out[i].h;
        CK(out[i].tile == i);
        CK(out[i].placed == 1);
        CK(out[i].w == ow[i]-2 && out[i].h == oh[i]-2);
        CK(out[i].src_x == 1 && out[i].src_y == 1);
        CK(out[i].x >= 0 && out[i].x + out[i].w <= 32);
        CK(out[i].y >= 0);
    }
    CK(st.saved_px == (int)(orig - trimmed));
    CK(st.height > 0);
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            CK(!overlaps(&out[i], &out[j]));

    /* trim == 0 must reproduce atlas_pack's shelf layout exactly */
    AtlasRect rr[N];
    AtlasPos pp[N];
    AtlasStats ss;
    for (int i = 0; i < N; ++i) { rr[i].w = ow[i]; rr[i].h = oh[i]; }
    CK(atlas_pack(rr, N, 32, pp, &ss) == 0);
    CK(atlas_build(ts, N, 32, 0, ATLAS_SHELF, out, &st, &bi) == ATLAS_OK);
    for (int i = 0; i < N; ++i) {
        CK(out[i].x == pp[i].x && out[i].y == pp[i].y);
        CK(out[i].w == ow[i] && out[i].h == oh[i]);
        CK(out[i].src_x == 0 && out[i].src_y == 0);
    }
    CK(st.used_px == ss.used_px);
    CK(st.height == ss.height);
    CK(st.saved_px == 0);

    /* greedy must not be worse than the shelf control here */
    AtlasStats g, sh;
    CK(atlas_build(ts, N, 32, 1, ATLAS_GREEDY, out, &g, &bi) == ATLAS_OK);
    CK(atlas_build(ts, N, 32, 1, ATLAS_SHELF, out, &sh, &bi) == ATLAS_OK);
    CK(g.used_px == sh.used_px);
    CK(g.height <= sh.height);

    /* trimmed width larger than the atlas -> EFIT names the index */
    unsigned char *w0 = mk_tile(40, 8, 5, 1, 30, 6);
    unsigned char *w1 = mk_tile(10, 8, 2, 1, 6, 5);
    AtlasTile wide[2] = {{40, 8, w0, 1}, {10, 8, w1, 1}};
    AtlasTrim tw[2];
    CK(atlas_trim(wide, 2, tw, &bi) == ATLAS_OK);
    CK(atlas_place(tw, 2, 20, ATLAS_GREEDY, out, &st, &bi) == ATLAS_EFIT);
    CK(bi == 0);

    /* dimension errors point at the bad tile/position */
    AtlasTile dim[2] = {{0, 8, NULL, 0}, {8, 8, NULL, 0}};
    CK(atlas_trim(dim, 2, tr2, &bi) == ATLAS_EDIM && bi == 0);
    AtlasTrim td[2] = {{0, 0, 0, 0, 4}, {1, 0, 0, 8, 4}};
    CK(atlas_place(td, 2, 32, ATLAS_SHELF, out, &st, &bi) == ATLAS_EDIM && bi == 0);

    /* global argument errors */
    CK(atlas_build(ts, 0, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_EINVAL && bi == -1);
    CK(atlas_build(ts, N, 0, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_EINVAL);
    CK(atlas_build(ts, N, 32, 1, (AtlasMode)7, out, &st, &bi) == ATLAS_EINVAL);

    /* wrapper propagates stage-1 errors with the tile index; a failed call
     * leaves every slot unplaced */
    unsigned char *hole = mk_tile(8, 8, 0, 0, -1, -1);
    AtlasTile beh[3] = {ts[0], {8, 8, hole, 0}, ts[1]};
    memset(out, 0, sizeof(out));
    CK(atlas_build(beh, 3, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_ETRANSPARENT);
    CK(bi == 1);
    CK(out[0].placed == 0 && out[2].placed == 0);

    AtlasTile bm[2] = {ts[0], ts[1]};
    bm[1].margin = 9;   /* bbox 1 px in from each side, 9 px margin escapes */
    CK(atlas_build(bm, 2, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_ETRIM_BOUNDS);
    CK(bi == 1);
    bm[1].margin = -3;
    CK(atlas_build(bm, 2, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_EMARGIN);
    CK(bi == 1);

    /* trim == 0 ignores margins entirely (no alpha scan, no escape) */
    bm[1].margin = 9;
    CK(atlas_build(bm, 2, 32, 0, ATLAS_GREEDY, out, &st, &bi) == ATLAS_OK);
    CK(out[1].w == 9 && out[1].h == 7);
    CK(st.saved_px == 0 && st.width == 32);

    /* a successful trimmed greedy build has stats consistent with the slots */
    CK(atlas_build(ts, N, 32, 1, ATLAS_GREEDY, out, &st, &bi) == ATLAS_OK);
    int sum_slots = 0, h_end = 0;
    for (int i = 0; i < N; ++i) {
        sum_slots += out[i].w * out[i].h;
        if (out[i].y + out[i].h > h_end) h_end = out[i].y + out[i].h;
    }
    CK(sum_slots == st.used_px);
    CK(h_end == st.height);
    CK(st.width == 32);

    for (int i = 0; i < N; ++i) free(pix[i]);
    free(a); free(full); free(empty); free(ok); free(hole);
    free(w0); free(w1);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
