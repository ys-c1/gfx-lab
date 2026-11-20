#define _POSIX_C_SOURCE 199309L
#include "atlas.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const AtlasOrient ORIENT_TABLE[ATLAS_ORIENTS] = {
    {   0, 0, 0 },
    {  90, 1, 1 },
    { 180, 0, 2 },
    { 270, 1, 3 },
};

const AtlasOrient *atlas_orient_table(void) { return ORIENT_TABLE; }

static int valid_angle(int angle) {
    return angle == 0 || angle == 90 || angle == 180 || angle == 270;
}

static long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Core order-preserving next-fit shelf pass for one orientation.
 * `swap` selects each rectangle's footprint. */
static int pack_pass(const AtlasRect *rects, int n, int width, int angle,
                     int swap, AtlasPos *out, AtlasStats *stats) {
    int cur_x = 0, cur_y = 0, shelf_h = 0, shelves = 1;
    int shelf_used_w = 0;   /* sum of rectangle widths on the open shelf */
    int shelf_tall_w = 0;   /* width of a widest tallest rect on the shelf */
    long long used = 0, gap_px = 0, bound = 0;

    long long t0 = monotonic_ns();
    for (int i = 0; i < n; ++i) {
        int rw = swap ? rects[i].h : rects[i].w;
        int rh = swap ? rects[i].w : rects[i].h;
        if (rw <= 0 || rh <= 0 || rw > width) {
            stats->err_index = i + 1; /* 1-based, survives the loop */
            stats->elapsed_ns = monotonic_ns() - t0;
            return ATLAS_E_ROT_FIT;
        }
        if (cur_x + rw > width) {
            /* Close the open shelf and account for its structural bound:
             *   gap waste  = g*h_s (exact)
             *   slack waste <= (u_s - w_t)*(h_s - 1), where w_t is the width
             *   of a widest tallest rectangle (its own slack is zero). */
            int g = width - cur_x;
            gap_px += (long long)g * shelf_h;
            bound += (long long)g * shelf_h +
                     (long long)(shelf_used_w - shelf_tall_w) * (shelf_h - 1);
            cur_y += shelf_h;
            cur_x = 0;
            shelf_h = 0;
            shelf_used_w = 0;
            shelf_tall_w = 0;
            ++shelves;
        }
        out[i].x = cur_x;
        out[i].y = cur_y;
        out[i].placed = 1;
        cur_x += rw;
        shelf_used_w += rw;
        if (rh > shelf_h) {
            shelf_h = rh;
            shelf_tall_w = rw;
        } else if (rh == shelf_h && rw > shelf_tall_w) {
            shelf_tall_w = rw;
        }
        used += (long long)rw * rh;
    }
    long long elapsed = monotonic_ns() - t0;

    int height = cur_y + shelf_h;
    long long area = (long long)width * height;
    long long waste = area - used;
    /* Tail gap and bound of the final, open shelf. */
    int tail_g = width - cur_x;
    gap_px += (long long)tail_g * shelf_h;
    bound += (long long)tail_g * shelf_h +
             (long long)(shelf_used_w - shelf_tall_w) * (shelf_h - 1);
    /* Identity over the packed region: waste = horizontal gap + vertical
     * slack (rectangles shorter than their shelf height). */
    long long slack_px = waste - gap_px;

    stats->width = width;
    stats->used_px = (int)used;
    stats->shelves = shelves;
    stats->angle = angle;
    stats->height = height;
    stats->area = area;
    stats->waste_px = waste;
    stats->waste_rate = area ? (double)waste / (double)area : 0.0;
    stats->gap_px = gap_px;
    stats->slack_px = slack_px;
    stats->bound_px = bound;
    stats->bound_rate = area ? (double)bound / (double)area : 0.0;
    stats->elapsed_ns = elapsed;
    stats->err_index = 0;
    return ATLAS_OK;
}

int atlas_pack_oriented(const AtlasRect *rects, int n, int width, int angle,
                        AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0)
        return ATLAS_E_ARG;
    if (!valid_angle(angle))
        return ATLAS_E_ANGLE;
    int swap = ORIENT_TABLE[(angle / 90) & 3].swap;
    return pack_pass(rects, n, width, angle, swap, out, stats);
}

int atlas_pack(const AtlasRect *rects, int n, int width,
               AtlasPos *out, AtlasStats *stats) {
    if (!rects || !out || !stats || n <= 0 || width <= 0)
        return ATLAS_E_ARG;
    /* Legacy contract: any rectangle whose own width exceeds the page is
     * ATLAS_E_FIT (-2), reported before any placement. */
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width)
            return ATLAS_E_FIT;
    }
    AtlasStats st;
    int rc = pack_pass(rects, n, width, 0, 0, out, &st);
    if (rc != ATLAS_OK)
        return ATLAS_E_FIT; /* only reachable via the same precondition */
    *stats = st;
    return ATLAS_OK;
}

int atlas_pack_best(const AtlasRect *rects, int n, int width, int allow_rotate,
                    AtlasPos *out, AtlasStats *stats,
                    AtlasStats passes[ATLAS_ORIENTS]) {
    if (!rects || !out || !stats || n <= 0 || width <= 0)
        return ATLAS_E_ARG;

    AtlasStats local[ATLAS_ORIENTS];
    AtlasStats *dst = passes ? passes : local;
    AtlasPos *scratch = (AtlasPos *)malloc((size_t)n * sizeof(AtlasPos));
    if (!scratch)
        return ATLAS_E_ARG;

    int count = allow_rotate ? ATLAS_ORIENTS : 1;
    int best = -1;
    int first_fail = -1;

    for (int k = 0; k < count; ++k) {
        const AtlasOrient *o = &ORIENT_TABLE[k];
        memset(&dst[k], 0, sizeof dst[k]);
        dst[k].angle = o->angle;
        /* Only the winner is backfilled into `out`; other passes render into
         * the scratch buffer so a later pass never clobbers output. */
        AtlasPos *buf = (best < 0) ? out : scratch;
        int rc = pack_pass(rects, n, width, o->angle, o->swap, buf, &dst[k]);
        if (rc != ATLAS_OK) {
            if (first_fail < 0) first_fail = k;
            continue;
        }
        if (best < 0 || dst[k].height < dst[best].height)
            best = k;
    }

    if (best < 0) {
        /* Every orientation failed: surface the first failing index. */
        free(scratch);
        *stats = dst[first_fail];
        return ATLAS_E_ROT_FIT;
    }

    if (best != 0) {
        /* Re-run the winner into `out` so public positions match the stats. */
        const AtlasOrient *o = &ORIENT_TABLE[best];
        int rc = pack_pass(rects, n, width, o->angle, o->swap, out, &dst[best]);
        free(scratch);
        if (rc != ATLAS_OK) {
            *stats = dst[best];
            return rc;
        }
    } else {
        free(scratch);
    }
    *stats = dst[best];
    return ATLAS_OK;
}
