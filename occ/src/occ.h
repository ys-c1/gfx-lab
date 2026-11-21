#ifndef OCC_H
#define OCC_H

/*
 * Occlusion-segment coverage, two phases:
 *   phase 1 (occ_compute):   rasterize every segment into its own mask,
 *                            one coverage rate per segment;
 *   phase 2 (occ_aggregate): OR-merge the per-segment masks into one mask.
 * With aggregation off, only phase 1 runs and no mask/rate is ever touched
 * by the merge path.
 *
 * Grid coordinates: cells span integer squares [x, x+1) x [y, y+1), so
 * valid segment endpoints are 0 <= x < w, 0 <= y < h. A segment whose
 * endpoint lies on the outer border (x == w or y == h), or outside it,
 * is reported as out-of-bounds together with its segment index. A
 * zero-length segment is legal input but rasterizes to an empty mask.
 *
 * Every failure carries the offending segment index through *bad_seg:
 *   OCC_EZERO  nseg == 0, *bad_seg is set to nseg (0);
 *   OCC_EOOB   segment *bad_seg reaches outside the grid;
 *   OCC_EEMPTY segment *bad_seg covers no cell;
 *   OCC_EMASK  the merged mask would be empty (nseg reported, no single
 *              segment can be blamed).
 */

#define OCC_OK      0
#define OCC_EARG   -1   /* null pointer / bad size / bad argument           */
#define OCC_EZERO  -2   /* nseg == 0                                        */
#define OCC_EOOB   -3   /* segment endpoint outside the grid                */
#define OCC_EEMPTY -4   /* segment rasterizes to an empty mask              */
#define OCC_EMASK  -5   /* aggregate mask empty / state not computed        */

typedef struct { float x0, y0, x1, y1; } OccSeg;

typedef struct {
    int w, h;            /* grid size in cells                       */
    int nseg;            /* number of segments                       */
    unsigned char *mask; /* nseg * w * h bytes, one plane per segment */
    float *rate;         /* nseg coverage rates: covered / (w*h)      */
    int *covered;        /* nseg covered-cell counts                 */
} OccCov;

typedef struct {
    unsigned char *mask; /* w * h union mask                          */
    int *added;          /* cells newly set by segment i (traceable)  */
    long or_ops;         /* OR attempts over every segment cell       */
    long union_cells;    /* cells set in the merged mask              */
} OccAgg;

/* Phase 1. cov holds per-segment masks/rates on OCC_OK. */
int  occ_compute(OccCov *cov, const OccSeg *segs, int nseg, int w, int h,
                 int *bad_seg);
void occ_cov_free(OccCov *cov);

/* Phase 2. Only valid after a successful occ_compute; never called when
 * aggregation is off, so phase-1 outputs stay byte-for-byte unchanged. */
int  occ_aggregate(const OccCov *cov, OccAgg *agg, int *bad_seg);
void occ_agg_free(OccAgg *agg);

/* Independent reference: clip each segment against every cell with the
 * Liang-Barsky test, mark a cell when the clipped interval is non-empty.
 * Same error contract as occ_compute (bad_seg included). */
int  occ_reference_mask(unsigned char *out, const OccSeg *segs, int nseg,
                        int w, int h, int *bad_seg);

const char *occ_strerror(int code);

/* PGM (P5) preview of one segment plane or the union mask. */
int  occ_write_pgm(const unsigned char *mask, int w, int h, const char *path);

#endif
