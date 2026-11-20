#include "atlas.h"
#include <stdio.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    /* ---- legacy behavior must stay intact ---- */
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);

    /* ---- orientation table is retrievable ---- */
    const AtlasOrient *t = atlas_orient_table();
    CK(t != 0);
    CK(t[0].angle == 0 && t[0].swap == 0 && t[0].quadrant == 0);
    CK(t[1].angle == 90 && t[1].swap == 1);
    CK(t[2].angle == 180 && t[2].swap == 0);
    CK(t[3].angle == 270 && t[3].swap == 1);

    /* ---- illegal angles ---- */
    CK(atlas_pack_oriented(r, 4, 64, 45, p, &s) == ATLAS_E_ANGLE);
    CK(atlas_pack_oriented(r, 4, 64, -90, p, &s) == ATLAS_E_ANGLE);
    CK(atlas_pack_oriented(r, 4, 64, 360, p, &s) == ATLAS_E_ANGLE);
    CK(atlas_pack_oriented(0, 4, 64, 0, p, &s) == ATLAS_E_ARG);

    /* ---- rotated rectangle exceeds page: error carries the index ---- */
    /* rect #2 is 40x16: at 90 deg its width becomes 16 (fits); make one that
     * only fails after rotation: 8x128 -> rotated width 128 > page 64. */
    AtlasRect rot_bad[3] = {{16,16},{8,128},{8,8}};
    CK(atlas_pack_oriented(rot_bad, 3, 64, 0, p, &s) == ATLAS_OK);
    CK(atlas_pack_oriented(rot_bad, 3, 64, 90, p, &s) == ATLAS_E_ROT_FIT);
    CK(s.err_index == 2);
    /* 270 swaps the same way and must point at the same index. */
    CK(atlas_pack_oriented(rot_bad, 3, 64, 270, p, &s) == ATLAS_E_ROT_FIT);
    CK(s.err_index == 2);
    /* 180 keeps the footprint, so it still fits. */
    CK(atlas_pack_oriented(rot_bad, 3, 64, 180, p, &s) == ATLAS_OK);

    /* ---- 90 and 270 produce identical footprints; 0 and 180 too ---- */
    AtlasStats s90, s270, s0, s180;
    AtlasRect q[4] = {{16,16},{32,8},{8,8},{64,4}};
    CK(atlas_pack_oriented(q, 4, 64, 0, p, &s0) == ATLAS_OK);
    CK(atlas_pack_oriented(q, 4, 64, 180, p, &s180) == ATLAS_OK);
    CK(atlas_pack_oriented(q, 4, 64, 90, p, &s90) == ATLAS_OK);
    CK(atlas_pack_oriented(q, 4, 64, 270, p, &s270) == ATLAS_OK);
    CK(s0.height == s180.height && s0.shelves == s180.shelves);
    CK(s90.height == s270.height && s90.shelves == s270.shelves);

    /* ---- best-of-four picks the minimum height and backfills ---- */
    AtlasStats passes[ATLAS_ORIENTS];
    AtlasPos bp[4];
    CK(atlas_pack_best(q, 4, 64, 1, bp, &s, passes) == ATLAS_OK);
    int min_h = passes[0].height;
    for (int k = 1; k < ATLAS_ORIENTS; ++k)
        if (passes[k].height < min_h) min_h = passes[k].height;
    CK(s.height == min_h);
    /* Hand check: angle 0 gives shelves [16,32,8](h16)+[64x4] H=20, while
     * 90 deg needs three shelves H=32, so the no-rotation layout wins. */
    CK(s.angle == 0);
    CK(s.height == 20 && s.shelves == 2);
    /* backfilled positions must agree with a fresh oriented run */
    AtlasPos ref[4];
    AtlasStats rs;
    CK(atlas_pack_oriented(q, 4, 64, s.angle, ref, &rs) == ATLAS_OK);
    for (int i = 0; i < 4; ++i)
        CK(bp[i].x == ref[i].x && bp[i].y == ref[i].y);

    /* ---- allow_rotate=0 reproduces the legacy layout exactly ---- */
    AtlasPos lp[4], gp[4];
    AtlasStats ls, gs;
    CK(atlas_pack_best(q, 4, 64, 0, lp, &ls, 0) == ATLAS_OK);
    CK(atlas_pack(q, 4, 64, gp, &gs) == ATLAS_OK);
    CK(ls.angle == 0);
    CK(ls.height == gs.height && ls.shelves == gs.shelves);
    for (int i = 0; i < 4; ++i)
        CK(lp[i].x == gp[i].x && lp[i].y == gp[i].y);

    /* ---- waste accounting and its bound ---- */
    CK(s.waste_px == s.area - s.used_px);
    CK(s.waste_rate >= 0.0 && s.waste_rate <= 1.0);
    CK(s.bound_px >= s.waste_px);
    CK(s.gap_px + s.slack_px == s.waste_px);
    for (int k = 0; k < ATLAS_ORIENTS; ++k)
        CK(passes[k].bound_px >= passes[k].waste_px);

    /* ---- all orientations fail: first failing index is reported ---- */
    /* 128x128 exceeds the 64-wide page in every orientation. */
    AtlasRect all_bad[2] = {{128,128},{128,128}};
    CK(atlas_pack_best(all_bad, 2, 64, 1, p, &s, passes) == ATLAS_E_ROT_FIT);
    CK(s.err_index == 1);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
