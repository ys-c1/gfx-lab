#ifndef ATLAS_H
#define ATLAS_H

#define ATLAS_MAX_TIERS 8
#define ATLAS_ERROR_MSG 128

typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;

typedef struct {
    int width;
    int used_px;      /* area actually occupied by (possibly scaled) tiles */
    int shelves;
    int height;       /* atlas height: bottom edge of the last shelf */
    long long bin_px; /* width * height */
    double occupancy; /* used_px / bin_px */
    double waste;     /* 1 - occupancy */
    int scale_tiers;  /* tiers that actually apply a ratio < 1 */
    int scaled_tiles; /* tiles whose applied ratio is < 1 */
} AtlasStats;

/* Tier ratios are indexed from the largest-area tier (0) to the
 * smallest-area tier (tier_count - 1).  Every ratio must be in (0, 1]. */
typedef struct {
    int enabled;
    int tier_count;
    double ratio[ATLAS_MAX_TIERS];
} AtlasScaleOpt;

typedef enum {
    ATLAS_OK = 0,
    ATLAS_EINVAL = -1,
    ATLAS_EFIT = -2,
    ATLAS_ERATIO = -3,
    ATLAS_ETIERS = -4,
    ATLAS_ENOMEM = -5
} AtlasCode;

typedef struct {
    int code;
    int index; /* offending tile or tier ordinal; -1 when not applicable */
    char message[ATLAS_ERROR_MSG];
} AtlasError;

typedef struct {
    int index;        /* index in the caller's rects array */
    int tier;         /* 0 = largest area ... tier_count - 1 */
    double ratio;     /* applied scale factor */
    AtlasRect src;    /* original size */
    AtlasRect scaled; /* size written to the atlas */
    AtlasPos pos;     /* placement in the atlas */
} AtlasTile;

typedef struct {
    AtlasTile *tiles; /* packing order */
    int count;
    int tier_count;   /* requested number of tiers */
    int tiers_used;   /* non-empty tiers */
} AtlasLayout;

/* Original single-stage shelf packer.  Kept byte-for-byte compatible. */
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);

/* Fill opt with enabled=1 and evenly spaced default ratios:
 * largest tier is scaled hardest and the smallest tier is left at 1.0. */
void atlas_scale_opt_init(AtlasScaleOpt *opt, int tier_count);

/* Two-stage packer: classify tiles into area tiers, scale their dimensions,
 * then repack in tier order.  With scaling disabled (or all ratios == 1)
 * the placement is identical to atlas_pack().
 *
 * layout may be NULL.  When non-NULL it must be zero-initialised by the
 * caller and released with atlas_layout_free().
 * err may be NULL; when non-NULL it reports the failing ordinal. */
int atlas_pack_tiered(const AtlasRect *rects, int n, int width,
                      const AtlasScaleOpt *opt, AtlasPos *out,
                      AtlasStats *stats, AtlasLayout *layout, AtlasError *err);

void atlas_layout_free(AtlasLayout *layout);

const char *atlas_strerror(int code);

/* Content scaling.  Scaled pixels use nearest-neighbour sampling, so every
 * scaled pixel is a copy of one original pixel.  The original bytes are
 * retained as well, which makes atlas_image_restore() byte-exact. */
typedef struct {
    int w, h;   /* original size */
    int sw, sh; /* scaled size */
    int channels;
    unsigned char *src;    /* retained original content */
    unsigned char *scaled; /* nearest-neighbour scaled content */
} AtlasTileImage;

int atlas_image_scale(AtlasTileImage *img, const unsigned char *src,
                      int w, int h, int channels, int sw, int sh);
int atlas_image_restore(const AtlasTileImage *img, unsigned char *out);
int atlas_image_restore_pixel(const AtlasTileImage *img, int x, int y,
                              unsigned char *out);
int atlas_image_sample(const AtlasTileImage *img, int x, int y,
                       unsigned char *out);
void atlas_image_free(AtlasTileImage *img);

#endif
