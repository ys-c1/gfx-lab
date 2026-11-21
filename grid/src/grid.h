#ifndef GRID_H
#define GRID_H

/*
 * Uniform-grid spatial partition with a two-pass merge step.
 *
 * Pass 1 inserts every object box into all grid cells its AABB overlaps.
 * An object spanning multiple cells is registered in each of them, which
 * makes downstream rendering draw it more than once (duplicate count).
 *
 * Pass 2 merges object boxes that overlap (transitive closure, union-find),
 * recomputes the merged AABBs and re-runs the insertion on the merged set,
 * eliminating duplicates for every connected cluster.
 */

#define GRID_MAX_OBJECTS 100000
#define GRID_MAX_CELLS   1000000   /* cols * rows must not exceed this */

typedef struct { double x, y, w, h; int layer; } GBox;

typedef struct {
    int group_count;      /* number of groups emitted after merging     */
    int merge_pairs;      /* overlapping object pairs found (pass 2)    */
    int duplicates;       /* sum(cells) - unique objects, pass-specific */
    long long candidates; /* sum of per-cell list lengths               */
    int *group_ids;       /* group id per original object, length n     */
    GBox *group_boxes;    /* merged AABB per group, length group_count  */
} GridStats;

typedef struct {
    int cols, rows;
    double x0, y0, w, h;          /* world bounds of the grid */
    int *cell_head;               /* length cols*rows, -1 = empty */
    int *next;                    /* length n, intrusive list links */
    int *ids;                     /* length n, object ids in cells  */
    int n;                        /* number of entries stored       */
    long long candidates;
} Grid;

void grid_stats_init(GridStats *s);
void grid_stats_free(GridStats *s);

/* Build an insertion grid for the given boxes. Returns 0 on success,
 * negative error code otherwise:
 *   -1 bad arguments / allocation failure
 *   -2 n > GRID_MAX_OBJECTS (limit is reported via grid_last_limit())
 *   -3 cols*rows > GRID_MAX_CELLS
 *   -4 degenerate grid (w/h <= 0 or cols/rows <= 0)
 *   -5 invalid box (w/h <= 0)
 */
int grid_build(const GBox *boxes, int n,
               double gx, double gy, double gw, double gh,
               int cols, int rows, Grid *out);

/* Count duplicate registrations: total cell memberships minus distinct
 * objects that ended up in at least one cell. */
int grid_duplicates(const Grid *g);
void grid_free(Grid *g);

/* Second-pass builder: assign every box to exactly ONE owner cell (the
 * cell containing its top-left anchor). Each id appears once, so the
 * resulting grid has zero duplicate registrations by construction.
 * Returns the same error codes as grid_build(). */
int grid_assign_unique(const GBox *boxes, int n,
                       double gx, double gy, double gw, double gh,
                       int cols, int rows, Grid *out);

/* Merge overlapping boxes (transitive closure via union-find) and fill
 * st->group_ids / st->group_boxes / st->group_count / st->merge_pairs.
 * Returns 0 or -1 on bad arguments / allocation failure. */
int grid_merge_groups(const GBox *boxes, int n, GridStats *st);

/* Human-readable name of the most recent limit error, e.g.
 * "GRID_MAX_OBJECTS=100000". Always returns a static string. */
const char *grid_last_limit(void);

#endif
