#include "atlas.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Errors                                                             */
/* ------------------------------------------------------------------ */

static void err_set(AtlasError *err, int code, int index, const char *fmt, ...) {
    if (!err) return;
    err->code = code;
    err->index = index;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

const char *atlas_strerror(int code) {
    switch (code) {
    case ATLAS_OK: return "ok";
    case ATLAS_EINVAL: return "invalid argument";
    case ATLAS_EFIT: return "tile does not fit the atlas width";
    case ATLAS_ERATIO: return "scale ratio out of range (0, 1]";
    case ATLAS_ETIERS: return "tier count out of range";
    case ATLAS_ENOMEM: return "out of memory";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Original single-stage packer                                       */
/* ------------------------------------------------------------------ */

static void stats_finish(AtlasStats *stats, int width, int used, int shelves,
                         int height, int scale_tiers, int scaled_tiles) {
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    stats->height = height;
    stats->bin_px = (long long)width * (long long)height;
    stats->occupancy = stats->bin_px ? (double)used / (double)stats->bin_px : 0.0;
    stats->waste = 1.0 - stats->occupancy;
    stats->scale_tiers = scale_tiers;
    stats->scaled_tiles = scaled_tiles;
}

int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0) return ATLAS_EINVAL;
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) return ATLAS_EFIT;
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats_finish(stats, width, used, shelves, cur_y + shelf_h, 0, 0);
    return ATLAS_OK;
}

/* ------------------------------------------------------------------ */
/* Tiering                                                            */
/* ------------------------------------------------------------------ */

void atlas_scale_opt_init(AtlasScaleOpt *opt, int tier_count) {
    if (!opt) return;
    memset(opt, 0, sizeof(*opt));
    opt->enabled = 1;
    opt->tier_count = tier_count;
    /* Largest tiles are halved, smallest tiles stay at 1.0, with a linear
     * ramp in between.  Callers who want a different curve can overwrite
     * ratio[] after this call. */
    for (int t = 0; t < tier_count && t < ATLAS_MAX_TIERS; ++t) {
        if (tier_count <= 1) opt->ratio[t] = 1.0;
        else opt->ratio[t] = 0.5 + 0.5 * (double)t / (double)(tier_count - 1);
    }
}

static long long rect_area(const AtlasRect *r) {
    return (long long)r->w * (long long)r->h;
}

typedef struct { long long area; int index; } AreaItem;

static int cmp_area_item(const void *pa, const void *pb) {
    const AreaItem *a = (const AreaItem *)pa, *b = (const AreaItem *)pb;
    if (a->area != b->area) return a->area > b->area ? -1 : 1;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

/* Equal-area tiles always share a tier.  Distinct areas are split into at
 * most tier_count contiguous blocks; block 0 holds the largest tiles. */
static int build_tiers(const AtlasRect *rects, int n, int tier_count,
                       int *tier_of, int *tiers_used) {
    AreaItem *items = (AreaItem *)malloc(sizeof(AreaItem) * (size_t)n);
    if (!items) return ATLAS_ENOMEM;
    for (int i = 0; i < n; ++i) {
        items[i].area = rect_area(&rects[i]);
        items[i].index = i;
    }
    qsort(items, (size_t)n, sizeof(AreaItem), cmp_area_item);

    int nareas = 0;
    for (int k = 0; k < n; ++k)
        if (k == 0 || items[k].area != items[k - 1].area) ++nareas;

    int used_tiers = nareas < tier_count ? nareas : tier_count;

    int group = 0;
    for (int k = 0; k < n; ++k) {
        if (k > 0 && items[k].area != items[k - 1].area) ++group;
        tier_of[items[k].index] = (int)((long long)group * used_tiers / nareas);
    }
    free(items);
    *tiers_used = used_tiers;
    return ATLAS_OK;
}

static int scaled_dim(int v, double ratio) {
    int s = (int)floor((double)v * ratio + 0.5);
    return s < 1 ? 1 : s;
}

static int cmp_tile_order(const void *pa, const void *pb) {
    const AtlasTile *a = (const AtlasTile *)pa, *b = (const AtlasTile *)pb;
    if (a->tier != b->tier) return a->tier < b->tier ? -1 : 1;
    long long aa = rect_area(&a->scaled), ab = rect_area(&b->scaled);
    if (aa != ab) return aa > ab ? -1 : 1;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

/* Shelf placement over an already ordered list.  Writes results by the
 * caller's original index so both orders expose the same output shape. */
static void shelf_pack(const AtlasTile *tiles, int n, int width,
                       AtlasPos *out, int *used, int *shelves, int *height) {
    int cur_x = 0, cur_y = 0, shelf_h = 0;
    int u = 0, sh = 1;
    for (int i = 0; i < n; ++i) {
        int w = tiles[i].scaled.w, h = tiles[i].scaled.h;
        if (cur_x + w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++sh;
        }
        out[tiles[i].index].x = cur_x;
        out[tiles[i].index].y = cur_y;
        out[tiles[i].index].placed = 1;
        cur_x += w;
        if (h > shelf_h) shelf_h = h;
        u += w * h;
    }
    *used = u;
    *shelves = sh;
    *height = cur_y + shelf_h;
}

int atlas_pack_tiered(const AtlasRect *rects, int n, int width,
                      const AtlasScaleOpt *opt, AtlasPos *out,
                      AtlasStats *stats, AtlasLayout *layout, AtlasError *err) {
    if (err) { err->code = ATLAS_OK; err->index = -1; err->message[0] = '\0'; }
    if (layout) { layout->tiles = NULL; layout->count = 0;
                  layout->tier_count = 0; layout->tiers_used = 0; }

    if (!rects || !out || !stats || n <= 0 || width <= 0) {
        err_set(err, ATLAS_EINVAL, -1, "null argument or non-positive size");
        return ATLAS_EINVAL;
    }

    int scaling = opt && opt->enabled;
    int tier_count = 1;
    if (scaling) {
        tier_count = opt->tier_count;
        if (tier_count <= 0) {
            err_set(err, ATLAS_ETIERS, 0,
                    "tier count %d is not positive; expected 1..%d", tier_count, ATLAS_MAX_TIERS);
            return ATLAS_ETIERS;
        }
        if (tier_count > ATLAS_MAX_TIERS) {
            err_set(err, ATLAS_ETIERS, 0,
                    "tier count %d exceeds maximum %d", tier_count, ATLAS_MAX_TIERS);
            return ATLAS_ETIERS;
        }
        for (int t = 0; t < tier_count; ++t) {
            double r = opt->ratio[t];
            if (!(r > 0.0 && r <= 1.0)) {
                err_set(err, ATLAS_ERATIO, t,
                        "scale ratio %.6g for tier %d out of range (0, 1]", r, t);
                return ATLAS_ERATIO;
            }
        }
    }

    int *tier_of = (int *)malloc(sizeof(int) * (size_t)n);
    AtlasTile *tiles = (AtlasTile *)malloc(sizeof(AtlasTile) * (size_t)n);
    if (!tier_of || !tiles) {
        free(tier_of); free(tiles);
        err_set(err, ATLAS_ENOMEM, -1, "out of memory");
        return ATLAS_ENOMEM;
    }

    /* Non-positive dimensions are never meaningful, even after scaling. */
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0) {
            err_set(err, ATLAS_EFIT, i,
                    "tile %d has non-positive size %dx%d", i, rects[i].w, rects[i].h);
            free(tier_of); free(tiles);
            return ATLAS_EFIT;
        }
    }

    int tiers_used = 1;
    if (scaling) {
        int rc = build_tiers(rects, n, tier_count, tier_of, &tiers_used);
        if (rc != ATLAS_OK) {
            err_set(err, rc, -1, "%s", atlas_strerror(rc));
            free(tier_of); free(tiles);
            return rc;
        }
    } else {
        for (int i = 0; i < n; ++i) tier_of[i] = 0;
    }

    int scaled_tiles = 0;
    for (int i = 0; i < n; ++i) {
        AtlasTile *t = &tiles[i];
        t->index = i;
        t->tier = tier_of[i];
        t->ratio = scaling ? opt->ratio[t->tier] : 1.0;
        t->src = rects[i];
        t->scaled.w = scaled_dim(rects[i].w, t->ratio);
        t->scaled.h = scaled_dim(rects[i].h, t->ratio);
        t->pos.x = t->pos.y = 0;
        t->pos.placed = 0;
        if (t->ratio < 1.0) ++scaled_tiles;
    }

    /* Tier-major order only when a ratio actually shrinks a tile.  With
     * scaling off, or with every ratio at 1.0, input order is preserved. */
    if (scaling && scaled_tiles > 0)
        qsort(tiles, (size_t)n, sizeof(AtlasTile), cmp_tile_order);

    for (int k = 0; k < n; ++k) {
        if (tiles[k].scaled.w > width) {
            err_set(err, ATLAS_EFIT, tiles[k].index,
                    "tile %d scaled width %d still exceeds atlas width %d",
                    tiles[k].index, tiles[k].scaled.w, width);
            free(tier_of); free(tiles);
            return ATLAS_EFIT;
        }
    }

    int used = 0, shelves = 0, height = 0;
    shelf_pack(tiles, n, width, out, &used, &shelves, &height);
    /* Report the number of tiers that actually apply a scale.  A ratio table
     * of all ones leaves the layout unscaled, hence zero applied tiers. */
    stats_finish(stats, width, used, shelves, height,
                 (scaling && scaled_tiles > 0) ? tiers_used : 0, scaled_tiles);

    if (layout) {
        layout->tiles = (AtlasTile *)malloc(sizeof(AtlasTile) * (size_t)n);
        if (!layout->tiles) {
            free(tier_of); free(tiles);
            err_set(err, ATLAS_ENOMEM, -1, "out of memory");
            return ATLAS_ENOMEM;
        }
        for (int k = 0; k < n; ++k)
            layout->tiles[tiles[k].index] = tiles[k];
        layout->count = n;
        layout->tier_count = scaling ? tier_count : 0;
        layout->tiers_used = scaling ? tiers_used : 0;
    }

    free(tier_of);
    free(tiles);
    return ATLAS_OK;
}

void atlas_layout_free(AtlasLayout *layout) {
    if (!layout) return;
    free(layout->tiles);
    layout->tiles = NULL;
    layout->count = 0;
    layout->tier_count = 0;
    layout->tiers_used = 0;
}

/* ------------------------------------------------------------------ */
/* Content scaling / exact restore                                    */
/* ------------------------------------------------------------------ */

int atlas_image_scale(AtlasTileImage *img, const unsigned char *src,
                      int w, int h, int channels, int sw, int sh) {
    if (!img) return ATLAS_EINVAL;
    memset(img, 0, sizeof(*img));
    if (!src || w <= 0 || h <= 0 || channels <= 0 || sw <= 0 || sh <= 0)
        return ATLAS_EINVAL;

    size_t src_bytes = (size_t)w * (size_t)h * (size_t)channels;
    size_t dst_bytes = (size_t)sw * (size_t)sh * (size_t)channels;
    img->src = (unsigned char *)malloc(src_bytes);
    img->scaled = (unsigned char *)malloc(dst_bytes);
    if (!img->src || !img->scaled) { atlas_image_free(img); return ATLAS_ENOMEM; }
    memcpy(img->src, src, src_bytes);
    img->w = w; img->h = h; img->sw = sw; img->sh = sh; img->channels = channels;

    /* Nearest-neighbour: every scaled pixel is a verbatim copy of one
     * original pixel, which is what makes the mapping verifiable. */
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            int sx = (int)(((long long)x * w) / sw);
            int sy = (int)(((long long)y * h) / sh);
            if (sx >= w) sx = w - 1;
            if (sy >= h) sy = h - 1;
            memcpy(img->scaled + ((size_t)y * sw + x) * channels,
                   img->src + ((size_t)sy * w + sx) * channels,
                   (size_t)channels);
        }
    return ATLAS_OK;
}

int atlas_image_restore(const AtlasTileImage *img, unsigned char *out) {
    if (!img || !img->src || !out || img->w <= 0 || img->h <= 0) return ATLAS_EINVAL;
    memcpy(out, img->src, (size_t)img->w * (size_t)img->h * (size_t)img->channels);
    return ATLAS_OK;
}

int atlas_image_restore_pixel(const AtlasTileImage *img, int x, int y,
                              unsigned char *out) {
    if (!img || !img->src || !out) return ATLAS_EINVAL;
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) return ATLAS_EINVAL;
    memcpy(out, img->src + ((size_t)y * img->w + x) * img->channels,
           (size_t)img->channels);
    return ATLAS_OK;
}

int atlas_image_sample(const AtlasTileImage *img, int x, int y,
                       unsigned char *out) {
    if (!img || !img->scaled || !out) return ATLAS_EINVAL;
    if (x < 0 || y < 0 || x >= img->sw || y >= img->sh) return ATLAS_EINVAL;
    memcpy(out, img->scaled + ((size_t)y * img->sw + x) * img->channels,
           (size_t)img->channels);
    return ATLAS_OK;
}

void atlas_image_free(AtlasTileImage *img) {
    if (!img) return;
    free(img->src); free(img->scaled);
    img->src = NULL; img->scaled = NULL;
    img->w = img->h = img->sw = img->sh = img->channels = 0;
}
