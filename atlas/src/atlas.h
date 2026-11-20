#ifndef ATLAS_H
#define ATLAS_H

#include <stddef.h>

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;

/* ---- Rotation orientation table --------------------------------------- */

#define ATLAS_ORIENTS 4

/* One row per sweep state. `swap` says whether w/h are interchanged:
 * 0/180 keep the footprint, 90/270 swap it. The table is the single source
 * of truth for both the sweep order and the angle->footprint mapping. */
typedef struct {
    int angle;     /* 0, 90, 180, 270 */
    int swap;      /* 1 => width/height interchanged */
    int quadrant;  /* sweep index 0..3 */
} AtlasOrient;

/* Always non-NULL, ATLAS_ORIENTS entries, ordered 0, 90, 180, 270. */
const AtlasOrient *atlas_orient_table(void);

/* ---- Stats ------------------------------------------------------------- */

/* First three fields keep their legacy meaning/types so existing callers
 * (atlas_pack) stay source- and behavior-compatible. Areas that can grow
 * beyond int range use long long. */
typedef struct {
    int width;
    int used_px;       /* total rectangle area, rotation-invariant */
    int shelves;       /* number of strips/bands used */
    int angle;         /* orientation of this run */
    int height;        /* total packed height H in px */
    long long area;    /* enclosing area = width * height */
    long long waste_px;/* area - used_px */
    double waste_rate; /* waste_px / area, in [0,1] */
    long long gap_px;  /* horizontal waste: sum of shelf gaps times shelf h */
    long long slack_px;/* vertical waste: unused height inside shelf rects */
    long long bound_px;/* per-shelf proven upper bound on waste_px:
                        * sum_s [ g_s*h_s + (u_s-w_t_s)*(h_s-1) ], where g_s
                        * is the shelf gap, u_s its total rect width, h_s its
                        * height, w_t_s the width of a widest tallest rect */
    double bound_rate; /* bound_px / area, in [0,1) */
    long long elapsed_ns; /* wall time of this run (CLOCK_MONOTONIC) */
    int err_index;     /* 1-based rect index on error, 0 otherwise */
} AtlasStats;

/* Return codes. */
enum {
    ATLAS_OK = 0,
    ATLAS_E_ARG = -1,    /* null pointer / n<=0 / width<=0 */
    ATLAS_E_FIT = -2,    /* legacy atlas_pack: rect wider than page */
    ATLAS_E_ANGLE = -3,  /* angle not one of 0/90/180/270 */
    ATLAS_E_ROT_FIT = -4 /* rect exceeds page width after rotation;
                          * stats.err_index holds its 1-based index */
};

/* ---- API --------------------------------------------------------------- */

/* Legacy entry point: unchanged order-preserving next-fit shelf packer,
 * no rotation. Layout is byte-for-byte identical to before. */
int atlas_pack(const AtlasRect *rects, int n, int width,
               AtlasPos *out, AtlasStats *stats);

/* Phase 1: one layout pass at a fixed angle.
 *  - ATLAS_E_ANGLE (-3) for an illegal angle.
 *  - ATLAS_E_ROT_FIT (-4) with stats.err_index = 1-based index of the first
 *    rectangle whose (possibly rotated) width exceeds the page. */
int atlas_pack_oriented(const AtlasRect *rects, int n, int width, int angle,
                        AtlasPos *out, AtlasStats *stats);

/* Phase 2: run all four orientations and backfill the best result.
 * "Best" = smallest packed height; ties keep the earlier table entry, so a
 * 0-degree layout wins an equal-height tie and 90 wins over 270.
 * allow_rotate=0 skips the sweep and produces the exact angle-0 layout.
 * passes[] may be NULL; otherwise it receives one AtlasStats per orientation
 * (failed passes carry err_index, zero height). If every orientation fails,
 * returns ATLAS_E_ROT_FIT and reports the first failing pass. */
int atlas_pack_best(const AtlasRect *rects, int n, int width, int allow_rotate,
                    AtlasPos *out, AtlasStats *stats,
                    AtlasStats passes[ATLAS_ORIENTS]);

#endif
