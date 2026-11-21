#ifndef ATLAS_REF_H
#define ATLAS_REF_H

/*
 * Reference-counted atlas (experimental, opt-in).
 *
 * The original shelf packer in atlas.h stays untouched.  This module is only
 * compiled into the picture when ATLAS_USE_REFCOUNT is non-zero, so turning the
 * switch off leaves the old path (and its output bytes) unchanged.
 *
 * Model
 *  - The atlas is split into square pages.  Page geometry and the budget are
 *    expressed in "points" (1x pixels); `scale` (1 or 2) multiplies the
 *    footprint of a sprite in points.  Physical page buffers are
 *    page_points*scale pixels on a side.
 *  - Every sprite id owns one aligned square block.  Block side is
 *    8 << level points (minimum 8x8).  A block stays in the index while its
 *    reference count is positive, and also after it drops to zero: an idle
 *    block is only reclaimed by atlas_ref_reset().
 *  - atlas_ref_reset() runs exactly two phases:
 *      phase 1  walk the index and mark every block whose refs == 0 idle;
 *      phase 2  wipe every page buffer as a whole and clear the index.
 *    Blocks that still hold references are reported as leaked and discarded.
 *
 * All statistics are in points unless the field name ends in `_bytes`.
 */

#ifndef ATLAS_USE_REFCOUNT
#define ATLAS_USE_REFCOUNT 0
#endif

#if ATLAS_USE_REFCOUNT

#include <stddef.h>

#define ATLAS_REF_CELL        8
#define ATLAS_REF_MAX_PAGES   8
#define ATLAS_REF_MAX_RECTS   1024
#define ATLAS_REF_MAX_POINTS  256

typedef enum {
    ATLAS_OK            = 0,
    ATLAS_ERR_ARG       = -1,
    ATLAS_ERR_CONFIG    = -2,
    ATLAS_ERR_BAD_RECT  = -3,
    ATLAS_ERR_SIZE      = -4,
    ATLAS_ERR_BUDGET    = -5,
    ATLAS_ERR_FRAGMENTED= -6,
    ATLAS_ERR_OOB       = -7,
    ATLAS_ERR_STALE     = -8,
    ATLAS_ERR_NOMEM     = -9,
    ATLAS_ERR_IO        = -10
} AtlasStatus;

typedef struct {
    AtlasStatus code;   /* ATLAS_OK, or the negative value returned */
    int index;          /* offending rect/block id, or -1 */
    int page;           /* offending page id, or -1 */
    int x, y;           /* offending point coordinate, or -1 */
    int detail;         /* extra number (capacity, expected size, ...) */
    const char *what;   /* static description */
} AtlasError;

typedef struct {
    int page_points;    /* page side in points, power of two, >= 8 */
    int max_pages;      /* page budget, 1 .. ATLAS_REF_MAX_PAGES */
    int max_rects;      /* valid sprite ids are 0 .. max_rects-1 */
} AtlasRefConfig;

typedef struct {
    int x, y;           /* block origin in points (1x units) */
    int w, h;           /* requested sprite size (1x units) */
    int block_id;       /* index slot / stable block id */
    int page;
} AtlasRefPos;

typedef struct {
    /* geometry */
    int scale;
    int page_points;
    int page_px;        /* page_points * scale */
    int page_bytes;     /* 3 * page_px * page_px */
    int cell_points;    /* ATLAS_REF_CELL */
    /* current layout */
    int pages;
    int index_len;      /* live index entries (live + idle blocks) */
    int index_cap;      /* slots available in the index */
    int live_blocks;    /* entries with refs > 0 */
    int idle_blocks;    /* entries with refs == 0, reclaimed by reset */
    int live_refs;      /* sum of all reference counts */
    int used_points;    /* allocated block area, idle blocks included */
    int requested_points; /* sum of sprite areas (w*h*scale*scale) */
    int padding_points; /* used_points - requested_points */
    int stat_error;     /* allocator waste; equal to padding_points */
    int budget_points;  /* max_pages * page_points * page_points */
    int free_points;    /* budget_points - used_points */
    int page_total_points; /* pages * page_points * page_points */
    int slack_points;   /* page_total_points - used_points */
    int balance_points; /* used + free - budget; must be 0 */
    int budget_bytes;
    int used_bytes;
    int free_bytes;
    int balance_bytes;  /* used_bytes + free_bytes - budget_bytes; must be 0 */
    int total_bytes;    /* bytes committed to logical pages */
    /* cumulative counters, not cleared by reset */
    int acquired;       /* new blocks allocated */
    int reacquired;     /* acquisitions merged into an existing entry */
    int released;       /* successful release calls */
    int release_zeroed; /* releases that took a block to refs == 0 */
    int samples;        /* atlas_ref_sample calls */
    int sample_hits;    /* resolved to a block with refs > 0 */
    int sample_misses;  /* resolved to an idle block, or no block at all */
    int oob_checks;     /* out-of-range accesses detected */
    int uploads;
    int resets;
    int reset_marked;   /* phase 1: blocks marked idle */
    int reset_leaked;   /* phase 2: blocks discarded with refs > 0 */
    int reset_index_cleared; /* phase 2: index entries cleared */
    int refs_total;     /* references ever taken */
} AtlasRefStats;

typedef struct AtlasRefAtlas AtlasRefAtlas;

AtlasRefAtlas *atlas_ref_create(const AtlasRefConfig *cfg, int scale, AtlasError *err);
void atlas_ref_destroy(AtlasRefAtlas *a);

/* Take one reference on rect_id, allocating a block on first use.  A second
 * acquire of the same id/size only bumps the reference count. */
int atlas_ref_acquire(AtlasRefAtlas *a, int rect_id, int w, int h,
                      AtlasRefPos *pos, AtlasError *err);

/* Drop one reference.  The block becomes idle at zero and stays in the index
 * until atlas_ref_reset(). */
int atlas_ref_release(AtlasRefAtlas *a, int rect_id, AtlasError *err);

/* Resolve rect_id to its block and count the sample. */
int atlas_ref_sample(AtlasRefAtlas *a, int rect_id, AtlasRefPos *pos, AtlasError *err);

/* Two-phase reset; `out` (optional) receives the post-reset snapshot. */
int atlas_ref_reset(AtlasRefAtlas *a, AtlasRefStats *out, AtlasError *err);

/* Block id covering a point, or -1 when the point is free.  Out-of-range
 * page/coordinate accesses are rejected with ATLAS_ERR_OOB. */
int atlas_ref_probe(AtlasRefAtlas *a, int page, int x, int y, AtlasError *err);

/* Check every index entry and the id map for consistency / OOB geometry. */
int atlas_ref_validate(const AtlasRefAtlas *a, AtlasError *err);

int atlas_ref_stats(const AtlasRefAtlas *a, AtlasRefStats *out);

/* Replace one logical page's RGB bytes (page_px * page_px * 3). */
int atlas_ref_upload(AtlasRefAtlas *a, int page, const unsigned char *rgb,
                     size_t bytes, AtlasError *err);

/* Read a backing page buffer.  Buffers stay addressable after reset (they are
 * zeroed but no longer logical pages) so the wipe can be verified. */
const unsigned char *atlas_ref_page_pixels(const AtlasRefAtlas *a, int page,
                                           size_t *bytes, AtlasError *err);

/* Deterministic text layout dump.  It contains layout state only, never the
 * cumulative counters, so a reset atlas that is repacked identically exports
 * byte-for-byte the same text as a fresh atlas. */
int atlas_ref_export(const AtlasRefAtlas *a, const char *path, AtlasError *err);

#endif /* ATLAS_USE_REFCOUNT */
#endif /* ATLAS_REF_H */
