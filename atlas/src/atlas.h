#ifndef ATLAS_H
#define ATLAS_H

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; int rotated; } AtlasPos;
typedef struct { int width; int used_px; int shelves; int height; } AtlasStats;

/* atlas_pack_ex option flags */
#define ATLAS_SORT_HEIGHT  1u  /* stage 1: reorder input by decreasing height */
#define ATLAS_ALLOW_ROTATE 2u  /* stage 2: allow 90-degree rotation per rect   */

/*
 * Three-stage shelf packer.
 *   stage 1: build the binning sequence (custom order, optionally height-sorted)
 *   stage 2: walk the sequence once and decide the orientation of every rect
 *   stage 3: walk it again, opening shelves by shelf height and backfilling x/y
 *
 * With flags == 0 and order == NULL the layout is identical to atlas_pack().
 *
 * order, when non-NULL, must be a permutation of [0, n): n original indices
 * giving the binning sequence. err_index (when non-NULL) receives the
 * 0-based index of the offending item on error:
 *   -1 bad argument
 *   -2 rect i has w/h <= 0, or it cannot fit the strip even after rotation
 *   -3 order[k] is out of range or repeats an earlier index; *err_index = k
 *   -4 internal allocation failure
 */
int atlas_pack_ex(const AtlasRect *rects, int n, int width, unsigned flags,
                  const int *order, AtlasPos *out, AtlasStats *stats,
                  int *err_index);

/* Original sequential shelf packer; equivalent to atlas_pack_ex(..., 0, NULL, ...). */
int atlas_pack(const AtlasRect *rects, int n, int width,
               AtlasPos *out, AtlasStats *stats);
#endif
