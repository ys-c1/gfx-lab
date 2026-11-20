#ifndef ATLAS_H
#define ATLAS_H

/* ---- legacy shelf packer ------------------------------------------- */

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;

typedef struct {
    int width;     /* atlas width */
    int height;    /* used shelf height */
    int used_px;   /* sum of packed rectangle areas */
    int saved_px;  /* transparent pixels dropped by trimming (0 when not trimming) */
    int shelves;
} AtlasStats;

int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* ---- two-stage trim + pack ----------------------------------------- */

/* Source tile. pixels are RGBA8, w*h*4 bytes.  rgba == NULL means the whole
 * tile is opaque (nothing to trim).  margin is the transparent border kept
 * around the opaque bounding box; it must be >= 0. */
typedef struct {
    int w, h;
    const unsigned char *rgba;
    int margin;
} AtlasTile;

/* Stage 1 result: the rectangle to pack and where to copy it from. */
typedef struct {
    int tile;              /* original tile index */
    int src_x, src_y;      /* top-left corner inside the source tile */
    int w, h;              /* trimmed size, margin included */
} AtlasTrim;

/* Stage 2 result: one atlas index entry, indexed like the input array. */
typedef struct {
    int tile;              /* original tile index */
    int x, y;              /* position in the atlas */
    int w, h;              /* placed (trimmed) size */
    int src_x, src_y;      /* copy origin inside the source tile */
    int placed;
} AtlasSlot;

typedef enum {
    ATLAS_SHELF = 0,       /* next-fit shelf, input order (legacy layout) */
    ATLAS_GREEDY = 1       /* first-fit decreasing height (FFDH) */
} AtlasMode;

enum {
    ATLAS_OK = 0,
    ATLAS_EINVAL = -1,     /* null/invalid arguments, bad mode, out of memory */
    ATLAS_EFIT = -2,       /* trimmed width does not fit the atlas */
    ATLAS_ETRANSPARENT = -3, /* tile has no pixel with alpha != 0 */
    ATLAS_ETRIM_BOUNDS = -4, /* bbox + margin leaves the source tile */
    ATLAS_EMARGIN = -5,    /* negative margin */
    ATLAS_EDIM = -6        /* non-positive tile/rect dimension */
};

/* Stage 1: scan alpha, compute each opaque bounding box, expand it by the
 * per-tile margin.  On error *bad_index names the offending tile (or -1 for
 * global argument errors).  *bad_index is -1 on success too. */
int atlas_trim(const AtlasTile *tiles, int n, AtlasTrim *out, int *bad_index);

/* Stage 2: place the trimmed rects into a strip of the given width and write
 * the atlas index back.  out[i] always corresponds to trims[i]; its .tile
 * field carries the original tile index.  On error *bad_index names the
 * offending input position. */
int atlas_place(const AtlasTrim *trims, int n, int width, AtlasMode mode,
                AtlasSlot *out, AtlasStats *stats, int *bad_index);

/* Convenience wrapper.  trim == 0 skips alpha scanning and ignores margins;
 * with ATLAS_SHELF it reproduces atlas_pack's layout exactly.  trim != 0 runs
 * atlas_trim then atlas_place; stats->saved_px is filled in. */
int atlas_build(const AtlasTile *tiles, int n, int width, int trim, AtlasMode mode,
                AtlasSlot *out, AtlasStats *stats, int *bad_index);

#endif
