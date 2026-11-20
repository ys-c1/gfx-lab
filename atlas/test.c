#include "atlas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void fill_image(unsigned char *buf, int w, int h, int ch) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c)
                buf[((size_t)y * w + x) * ch + c] =
                    (unsigned char)((x * 37 + y * 91 + c * 13 + (x ^ y)) & 0xff);
}

static int same_positions(const AtlasPos *a, const AtlasPos *b, int n) {
    for (int i = 0; i < n; ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].placed != b[i].placed)
            return 0;
    return 1;
}

static void test_legacy_unchanged(void) {
    AtlasRect r[6] = {{16,16},{32,8},{8,8},{64,4},{24,12},{40,40}};
    AtlasPos legacy[6];
    AtlasStats ls;
    CK(atlas_pack(r, 6, 128, legacy, &ls) == 0);

    /* Expected legacy placement, computed by hand from the original code. */
    CK(legacy[0].x == 0  && legacy[0].y == 0);
    CK(legacy[1].x == 16 && legacy[1].y == 0);
    CK(legacy[2].x == 48 && legacy[2].y == 0);
    CK(legacy[3].x == 56 && legacy[3].y == 0);
    CK(legacy[4].x == 0  && legacy[4].y == 16); /* 24 does not fit after 56+64=120 */
    CK(ls.used_px == 16*16 + 32*8 + 8*8 + 64*4 + 24*12 + 40*40);
    CK(legacy[5].x == 24 && legacy[5].y == 16);
    CK(ls.shelves == 2);
    CK(ls.height == 56);
    CK(ls.bin_px == 128 * 56);
    CK(ls.scale_tiers == 0);

    /* Disabled scaling must reproduce the legacy layout byte for byte. */
    AtlasScaleOpt opt;
    atlas_scale_opt_init(&opt, 4);
    opt.enabled = 0;
    AtlasPos noopt[6], off[6];
    AtlasStats s1, s2;
    AtlasError err;
    CK(atlas_pack_tiered(r, 6, 128, NULL, noopt, &s1, NULL, &err) == 0);
    CK(atlas_pack_tiered(r, 6, 128, &opt, off, &s2, NULL, &err) == 0);
    CK(same_positions(legacy, noopt, 6));
    CK(same_positions(legacy, off, 6));
    CK(s1.used_px == ls.used_px && s1.shelves == ls.shelves && s1.height == ls.height);
    CK(s2.used_px == ls.used_px && s2.shelves == ls.shelves && s2.height == ls.height);
    CK(s1.scale_tiers == 0 && s2.scale_tiers == 0);

    /* Ratio table of all ones also leaves the layout untouched. */
    atlas_scale_opt_init(&opt, 3);
    for (int t = 0; t < 3; ++t) opt.ratio[t] = 1.0;
    AtlasPos ones[6];
    AtlasStats s3;
    CK(atlas_pack_tiered(r, 6, 128, &opt, ones, &s3, NULL, &err) == 0);
    CK(same_positions(legacy, ones, 6));
    CK(s3.scaled_tiles == 0);
    CK(s3.scale_tiers == 0);
}

static void test_tiering(void) {
    /* Four distinct areas, all large enough to shrink to > 6 px. */
    AtlasRect r[4] = {{80,80},{40,40},{20,20},{10,10}};
    AtlasScaleOpt opt = {1, 4, {0.5, 0.7, 0.85, 1.0}};
    AtlasPos p[4];
    AtlasStats s;
    AtlasLayout layout = {0};
    AtlasError err;
    CK(atlas_pack_tiered(r, 4, 256, &opt, p, &s, &layout, &err) == 0);
    CK(err.code == ATLAS_OK);
    CK(layout.count == 4);
    CK(layout.tier_count == 4);
    CK(layout.tiers_used == 4);
    CK(s.scale_tiers == 4);

    /* Largest tile maps to tier 0 / ratio 0.5, smallest stays tier 3 / 1.0. */
    CK(layout.tiles[0].tier == 0 && layout.tiles[0].ratio == 0.5);
    CK(layout.tiles[1].tier == 1 && layout.tiles[1].ratio == 0.7);
    CK(layout.tiles[2].tier == 2 && layout.tiles[2].ratio == 0.85);
    CK(layout.tiles[3].tier == 3 && layout.tiles[3].ratio == 1.0);
    CK(layout.tiles[0].scaled.w == 40 && layout.tiles[0].scaled.h == 40);
    CK(layout.tiles[1].scaled.w == 28 && layout.tiles[1].scaled.h == 28);
    CK(layout.tiles[2].scaled.w == 17 && layout.tiles[2].scaled.h == 17);
    CK(layout.tiles[3].scaled.w == 10 && layout.tiles[3].scaled.h == 10);
    CK(layout.tiles[0].src.w == 80 && layout.tiles[0].src.h == 80);

    /* Tier order is preserved in the packing order: big tiles first. */
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b)
            CK(!(layout.tiles[a].tier > layout.tiles[b].tier));

    CK(s.used_px == 40*40 + 28*28 + 17*17 + 10*10);
    CK(s.scaled_tiles == 3);
    CK(s.shelves == 1);
    CK(s.height == 40);
    CK(s.bin_px == 256LL * 40);
    CK(s.occupancy > 0.0 && s.occupancy <= 1.0);
    CK(s.waste + s.occupancy > 0.999999 && s.waste + s.occupancy < 1.000001);
    atlas_layout_free(&layout);
    CK(layout.tiles == NULL && layout.count == 0);

    /* Fewer distinct areas than requested tiers: empty tiers collapse. */
    AtlasRect same[3] = {{16,16},{16,16},{16,16}};
    AtlasScaleOpt opt4 = {1, 4, {0.5, 0.7, 0.85, 1.0}};
    AtlasLayout l2 = {0};
    AtlasStats s2;
    CK(atlas_pack_tiered(same, 3, 64, &opt4, p, &s2, &l2, &err) == 0);
    CK(l2.tiers_used == 1);
    CK(s2.scale_tiers == 1);
    for (int i = 0; i < 3; ++i) CK(l2.tiles[i].tier == 0);
    atlas_layout_free(&l2);
}

static void test_errors(void) {
    AtlasRect r[3] = {{32,32},{16,16},{8,8}};
    AtlasPos p[3];
    AtlasStats s;
    AtlasError err;

    /* Zero tier count: error names ordinal 0. */
    AtlasScaleOpt zero = {1, 0, {0.5, 0.5, 0.5}};
    CK(atlas_pack_tiered(r, 3, 128, &zero, p, &s, NULL, &err) == ATLAS_ETIERS);
    CK(err.code == ATLAS_ETIERS);
    CK(err.index == 0);
    CK(strlen(err.message) > 0);

    /* Negative tier count is just as invalid. */
    AtlasScaleOpt neg = {1, -2, {0.5, 0.5, 0.5}};
    CK(atlas_pack_tiered(r, 3, 128, &neg, p, &s, NULL, &err) == ATLAS_ETIERS);
    CK(err.index == 0);

    /* Ratio above 1: ordinal is the offending tier. */
    AtlasScaleOpt hi = {1, 3, {0.5, 1.25, 1.0}};
    CK(atlas_pack_tiered(r, 3, 128, &hi, p, &s, NULL, &err) == ATLAS_ERATIO);
    CK(err.code == ATLAS_ERATIO);
    CK(err.index == 1);
    CK(strstr(err.message, "tier 1") != NULL);

    /* Ratio of zero. */
    AtlasScaleOpt lo = {1, 3, {0.5, 0.8, 0.0}};
    CK(atlas_pack_tiered(r, 3, 128, &lo, p, &s, NULL, &err) == ATLAS_ERATIO);
    CK(err.index == 2);

    /* Negative ratio. */
    AtlasScaleOpt negram = {1, 2, {-0.1, 1.0}};
    CK(atlas_pack_tiered(r, 3, 128, &negram, p, &s, NULL, &err) == ATLAS_ERATIO);
    CK(err.index == 0);

    /* Too-wide tile with scaling off reports its tile index. */
    AtlasRect wide[2] = {{16,16},{200,8}};
    AtlasPos wp[2];
    CK(atlas_pack_tiered(wide, 2, 64, NULL, wp, &s, NULL, &err) == ATLAS_EFIT);
    CK(err.index == 1);
    CK(atlas_pack(wide, 2, 64, wp, &s) == -2);

    /* Scaling can rescue a tile that is too wide at full size. */
    AtlasScaleOpt half = {1, 1, {0.5}};
    CK(atlas_pack_tiered(wide, 2, 128, &half, wp, &s, NULL, &err) == 0);
    CK(err.code == ATLAS_OK);

    /* Zero/invalid input still maps to the original error code. */
    CK(atlas_pack_tiered(r, 0, 128, NULL, p, &s, NULL, &err) == ATLAS_EINVAL);
    CK(err.index == -1);
    CK(atlas_pack_tiered(r, 3, 0, NULL, p, &s, NULL, &err) == ATLAS_EINVAL);
    CK(atlas_pack_tiered(r, 3, 128, NULL, NULL, &s, NULL, &err) == ATLAS_EINVAL);

    CK(strcmp(atlas_strerror(ATLAS_ETIERS), "tier count out of range") == 0);
    CK(strcmp(atlas_strerror(ATLAS_ERATIO), "scale ratio out of range (0, 1]") == 0);
    CK(strcmp(atlas_strerror(42), "unknown error") == 0);
}

static void test_pixel_restore(void) {
    enum { W = 37, H = 23, CH = 3, SW = 19, SH = 12 };
    unsigned char *src = (unsigned char *)malloc((size_t)W * H * CH);
    unsigned char *restored = (unsigned char *)malloc((size_t)W * H * CH);
    unsigned char px[CH], ref[CH];
    fill_image(src, W, H, CH);

    AtlasTileImage img;
    CK(atlas_image_scale(&img, src, W, H, CH, SW, SH) == 0);
    CK(img.w == W && img.h == H && img.sw == SW && img.sh == SH && img.channels == CH);

    /* Whole-image restore is byte exact. */
    CK(atlas_image_restore(&img, restored) == 0);
    CK(memcmp(restored, src, (size_t)W * H * CH) == 0);

    /* Every scaled pixel is a verbatim copy of the documented source pixel. */
    int exact = 1;
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x) {
            int sx = x * W / SW, sy = y * H / SH;
            atlas_image_sample(&img, x, y, px);
            memcpy(ref, src + ((size_t)sy * W + sx) * CH, CH);
            if (memcmp(px, ref, CH) != 0) exact = 0;
        }
    CK(exact);

    /* Per-pixel restore recovers the original grid exactly. */
    int per_pixel_ok = 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            atlas_image_restore_pixel(&img, x, y, px);
            memcpy(ref, src + ((size_t)y * W + x) * CH, CH);
            if (memcmp(px, ref, CH) != 0) per_pixel_ok = 0;
        }
    CK(per_pixel_ok);

    /* Out-of-range coordinates are rejected instead of reading past bounds. */
    CK(atlas_image_sample(&img, -1, 0, px) == ATLAS_EINVAL);
    CK(atlas_image_sample(&img, SW, 0, px) == ATLAS_EINVAL);
    CK(atlas_image_restore_pixel(&img, W, 0, px) == ATLAS_EINVAL);

    /* The retained source is an independent copy. */
    src[0] ^= 0xff;
    CK(atlas_image_restore(&img, restored) == 0);
    CK(restored[0] != src[0]);

    atlas_image_free(&img);
    CK(img.src == NULL && img.scaled == NULL);
    free(src);
    free(restored);
}

int main(void) {
    test_legacy_unchanged();
    test_tiering();
    test_errors();
    test_pixel_restore();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
