#ifndef ATLAS_H
#define ATLAS_H

typedef struct { int w, h; } AtlasRect;

/* rot is the orientation actually used: 0 or 90 degrees. */
typedef struct { int x, y; int placed; int rot; } AtlasPos;

typedef struct {
    int width;
    int used_px;   /* sum of the source rect areas */
    int shelves;   /* shelf mode: shelf rows; maxrect mode: distinct top edges */
    int height;    /* strip height actually used (bounded mode: the bound) */
    int rotated;   /* rects placed at 90 degrees (maxrect only) */
    int free_rects;/* live maximal free rects after packing (maxrect only) */
    double waste;  /* 1 - used_px / (width * height) */
} AtlasStats;

typedef enum {
    ATLAS_SHELF = 0,   /* legacy sequential shelf packing */
    ATLAS_MAXRECT = 1  /* maximal free rects, best of 0/90 degrees */
} AtlasMode;

/* Per-rect orientation hint used by AtlasJob.orient; ATLAS_ROT_ANY lets the
 * packer try both orientations.  Any other value is an orientation error. */
#define ATLAS_ROT_ANY (-1)
#define ATLAS_ROT_0    0
#define ATLAS_ROT_90   90

typedef struct {
    int code;   /* 0 on success, negative ATLAS_ERR_* otherwise */
    int index;  /* offending rect index, -1 when the error is global */
} AtlasError;

#define ATLAS_ERR_ARGS     (-1) /* bad pointer / count / width / mode */
#define ATLAS_ERR_RECT     (-2) /* non-positive rect dimension */
#define ATLAS_ERR_TOO_BIG  (-3) /* rect does not fit the strip in any orientation */
#define ATLAS_ERR_EMPTY    (-4) /* free table is empty when a rect still needs space */
#define ATLAS_ERR_NO_FIT   (-5) /* free table is non-empty but no free rect fits */
#define ATLAS_ERR_ORIENT   (-6) /* illegal orientation hint, or pinned orientation does not fit */

typedef struct {
    const AtlasRect *rects;
    int n;
    int width;
    int height;        /* <= 0: unbounded strip */
    AtlasMode mode;
    const int *orient; /* optional, n entries, NULL means ATLAS_ROT_ANY for all */
} AtlasJob;

/* Legacy shelf packer: layout is unchanged from the original implementation.
 * Returns 0, -1 on bad arguments, -2 for a non-positive rect or one wider
 * than the strip. */
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* Maximal-rectangles packer on an unbounded strip.  On failure returns an
 * ATLAS_ERR_* code, sets err->index to the offending rect and leaves out
 * partially written. */
int atlas_pack_maxrect(const AtlasRect *rects, int n, int width, AtlasPos *out,
                       AtlasStats *stats, AtlasError *err);

/* General entry point; height > 0 bounds the strip, orient may be NULL. */
int atlas_pack_job(const AtlasJob *job, AtlasPos *out, AtlasStats *stats, AtlasError *err);

/* Best-case waste for an unbounded strip, from the area / largest-min-side
 * bound.  Any packing has at least this waste; returns -1.0 on bad input. */
double atlas_waste_lower_bound(const AtlasRect *rects, int n, int width);

/* Guaranteed worst case: NFDH packs any set in height <= 2*area/width +
 * max_height (rotating each rect to its shorter side when that fits), so some
 * packing has at most this waste.  Returns -1.0 when a rect cannot fit the
 * strip at all. */
double atlas_waste_upper_bound(const AtlasRect *rects, int n, int width);

/* Deterministic synthetic item set for demos/benchmarks: both sides are drawn
 * from [min_side, max_side] and the long side is split evenly between width
 * and height, so every width in that range can pack the set.  Values depend
 * only on seed and index, so shelf and maxrect see identical rects. */
void atlas_make_rects(AtlasRect *rects, int n, int min_side, int max_side,
                      unsigned int seed);

#endif
