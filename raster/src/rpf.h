#ifndef RPF_H
#define RPF_H
#include "raster.h"

/*
 * Self-intersecting polygon fill, split into two explicit phases:
 *
 *   phase 1 (coverage): fan triangulation (0, i, i+1) records, per pixel,
 *                       which fan triangles cover its centre (triangle ids).
 *                       Inside/outside is decided independently and
 *                       analytically: a horizontal ray from each pixel
 *                       centre yields the crossing count (parity) and the
 *                       signed winding number. The ray definition stays
 *                       correct for self-intersecting polygons, where the
 *                       signed-fan winding identity does not hold.
 *
 *   phase 2 (shading):  a rule is applied to the coverage record:
 *                         RPF_RULE_EVEN_ODD : inside when parity is odd
 *                         RPF_RULE_NONZERO  : inside when winding != 0
 *                       Only phase 2 ever writes the RTarget. With
 *                       RPF_MODE_OFF the shading phase is skipped and the
 *                       target stays byte-for-byte unchanged (legacy output).
 *
 * Pixel centres are sampled at (x + 0.5, y + 0.5) with the same top-left
 * convention as rt_triangle() (strict edge-function inequality, half-open
 * edges fall out of the strict test).
 */

#define RPF_MAX_VERTS 16
#define RPF_MAX_TRIS  (RPF_MAX_VERTS - 2)
#define RPF_MAX_STATES 32

enum { RPF_RULE_EVEN_ODD = 0, RPF_RULE_NONZERO = 1 };
enum { RPF_MODE_OFF = 0, RPF_MODE_ENABLED = 1 };

typedef struct {
    const RVert *verts;   /* polygon vertices, closed implicitly */
    int n;                /* 3 .. RPF_MAX_VERTS */
} RPoly;

typedef struct {
    int rule;             /* RPF_RULE_EVEN_ODD / RPF_RULE_NONZERO */
    int mode;             /* RPF_MODE_OFF / RPF_MODE_ENABLED */
    double z;             /* depth written by the shading phase */
    const unsigned char *eo_color;   /* 3 bytes, even-odd fill color */
    const unsigned char *pos_color;  /* 3 bytes, winding > 0 color */
    const unsigned char *neg_color;  /* 3 bytes, winding < 0 color */
} RPFillOpts;

typedef struct {
    unsigned char parity;                 /* ray crossing count (odd => inside EO) */
    signed char winding;                  /* analytic winding number */
    unsigned char ntris;                  /* number of valid tri_id entries */
    unsigned char overflow;               /* more than RPF_MAX_TRIS covers */
    unsigned char tri_id[RPF_MAX_TRIS];   /* fan triangle indices (0-based) */
} RPFCover;

typedef struct {
    int grid_w, grid_h;
    RPFCover *cover;       /* per-pixel phase-1 records, free with rpf_stats_free */
    int ntris;
    int covered_pixels;       /* pixels with parity > 0 */
    int eo_fill_pixels;       /* pixels filled under even-odd */
    int nz_fill_pixels;       /* pixels filled under non-zero */
    int tri_cover[RPF_MAX_TRIS]; /* pixels covered per fan triangle */
    int overflow_pixels;
    int nstates;               /* distinct (parity, winding) states present */
    double phase1_ms, phase2_ms, total_ms;
} RPFStats;

void rpf_stats_free(RPFStats *st);

typedef struct {
    unsigned char parity;
    signed char winding;
    int eo_inside;            /* 1 if even-odd fills this state */
    int nz_inside;            /* 1 if non-zero fills this state */
    int selected;             /* rule chosen by the current options */
    unsigned char rgb[3];
    int count;                /* pixels in this state */
} RPFState;

/* Run both phases. stats may be NULL. Returns fill pixel count, < 0 on error. */
int rpf_polygon(RTarget *t, const RPoly *poly, const RPFillOpts *opts,
                RPFStats *stats);

/* Per-pixel coverage query (phase-1 record). < 0 on bad field. */
int rpf_pixel_get(const RTarget *t, const RPFStats *st, int x, int y,
                  RPFCover *out);

/* Distinct (parity, winding) states actually present, i.e. the rule table. */
int rpf_rule_table(const RTarget *t, const RPFStats *st,
                   const RPFillOpts *opts, RPFState *out, int maxn);

/* Debug maps: parity PGM (0/255) and winding PGM (0, or 255-|w|). */
int rpf_write_parity_pgm(const RTarget *t, const RPFStats *st, const char *path);
int rpf_write_winding_pgm(const RTarget *t, const RPFStats *st, const char *path);

/* Human-readable report (config, counts, per-triangle table, rule table). */
int rpf_write_report(const RTarget *t, const RPFStats *st,
                     const RPoly *poly, const RPFillOpts *opts, const char *path);

/* Validation diagnostics: field path of the last rejected call, else NULL. */
const char *rpf_last_error(char *buf, int cap);

#endif
