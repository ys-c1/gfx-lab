#ifndef SHADOW_H
#define SHADOW_H

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/*
 * Cast-distance truncation
 * ------------------------
 * A point light casts an infinitely long shadow: a blocker at any distance
 * darkens every pixel on the ray behind it, so far-away receivers collect
 * speckle from many distant blockers.  Rendering is split into two stages:
 *
 *   stage A  sh_truncate()          clip every blocker segment to the part
 *                                   whose distance from the light is within
 *                                   `cutoff`, splitting it at the distance
 *                                   tier edges.  Pieces whose whole extent is
 *                                   farther than `cutoff` are dropped.
 *
 *   stage B  sh_render_truncated()  re-cast shadows from the truncated pieces
 *                                   only.  Receivers farther than `cutoff`
 *                                   from the light are never shadowed.
 *
 * Stage A is lossless for this semantics: a blocker farther from the light
 * than a receiver cannot lie between the two, so dropping it can never change
 * an in-range receiver.  Stage B therefore matches a direct per-pixel
 * evaluation of the original segments under the same distance cutoff.
 *
 * With `enable == 0` the whole feature is bypassed: sh_render_cast() calls the
 * untouched sh_render() verbatim, so the map (and the PGM written from it) is
 * byte-for-byte identical to the pre-truncation implementation.
 *
 * Distance tiers are receiver-distance bands (0, split_0], (split_0, split_1],
 * ... (split_{T-2}, cutoff].  A tier has one brightness value: a shadowed
 * pixel in tier k is shaded as `lit * bright[k]` (0 = black as before,
 * 1 = fully lit).  Tiers are reported by sh_truncate() and sh_render_truncated()
 * so a caller can see the piece count and mean luminance of each band.
 */

#define SH_MAX_TIERS 4

/* Return codes.  SH_ERR_NULL and SH_ERR_LIGHT keep the codes that the
 * original sh_render() already returns (-1 / -2). */
enum {
    SH_OK            =  0,
    SH_ERR_NULL      = -1,  /* required pointer NULL                        */
    SH_ERR_LIGHT     = -2,  /* radius/intensity/position invalid            */
    SH_ERR_TYPE      = -3,  /* value type does not match the field type     */
    SH_ERR_FIELD     = -4,  /* unknown field id                             */
    SH_ERR_RANGE     = -5,  /* value outside its allowed range              */
    SH_ERR_ORDER     = -6,  /* tier edges not ascending / past cutoff       */
    SH_ERR_MEM       = -7,  /* allocation failure                           */
    SH_ERR_IO        = -8,  /* PGM write failure                            */
    SH_ERR_STATE     = -9   /* operation invalid in the current state       */
};

/* On failure `code` is one of the codes above, `field` points at a static
 * field name ("cutoff", "bright_1", ...) when the error is tied to a field,
 * and `msg` spells out the offending value. */
typedef struct {
    int code;
    const char *field;
    char msg[192];
} ShError;

/* Declared value type carried by sh_cfg_set().  C has no runtime types, so
 * the caller states the type of `value`; a mismatch is rejected and the field
 * name is reported (SH_ERR_TYPE). */
typedef enum {
    SH_TY_NONE  = 0,
    SH_TY_INT   = 1,
    SH_TY_FLOAT = 2
} ShType;

/* Every field has exactly one type. */
typedef enum {
    SH_F_ENABLE = 0,   /* int   */
    SH_F_TIER_COUNT,   /* int   */
    SH_F_CUTOFF,       /* float */
    SH_F_SPLIT_0,      /* float */
    SH_F_SPLIT_1,      /* float */
    SH_F_SPLIT_2,      /* float */
    SH_F_BRIGHT_0,     /* float */
    SH_F_BRIGHT_1,     /* float */
    SH_F_BRIGHT_2,     /* float */
    SH_F_BRIGHT_3,     /* float */
    SH_F_COUNT
} ShField;

typedef struct {
    int   enable;                     /* 0 = bypass, byte-identical output */
    int   tier_count;                 /* 1..SH_MAX_TIERS                   */
    float cutoff;                     /* max cast distance, pixels, > 0    */
    float split[SH_MAX_TIERS - 1];    /* first tier_count-1 internal edges */
    float bright[SH_MAX_TIERS];       /* shadow brightness per receiver tier */
} ShTruncCfg;

/* One truncated blocker piece (stage A output).  `seg` is in map coordinates;
 * d0/d1 are the light distances of its endpoints; `tier` is the distance band
 * the piece lies in; `src` is the input segment index. */
typedef struct {
    ShSeg seg;
    int   src;
    int   tier;
    float d0, d1;
} ShCastPiece;

typedef struct {
    ShCastPiece *pieces;
    int   count;          /* truncated piece count ("截断段数")        */
    int   input_count;    /* input segment count                      */
    int   dropped;        /* segments with no piece inside cutoff     */
    int   split;          /* segments that produced >1 piece          */
    int   clipped;        /* segments that lost at least one endpoint */
    int   per_tier[SH_MAX_TIERS];
    int   tier_count;
    float cutoff;
    float split_edge[SH_MAX_TIERS - 1];
    float bright[SH_MAX_TIERS];
    double len_before;    /* total input blocker length               */
    double len_after;     /* total kept blocker length                */
} ShCastSet;

/* Per-render measurements (stage B), reported per receiver tier. */
typedef struct {
    long   pixels[SH_MAX_TIERS];
    double lum_sum[SH_MAX_TIERS];
    long   shadowed[SH_MAX_TIERS];
    long   beyond_cutoff;      /* pixels farther than cutoff (never shadowed) */
    double lum_sum_total;
    long   pixels_total;
    long   shadowed_total;
    long   piece_tests;
} ShRenderStats;

const char *sh_field_name(ShField f);      /* NULL if f is not a field    */
ShType      sh_field_type(ShField f);      /* SH_TY_NONE if f is unknown  */
void        sh_error_clear(ShError *err);

/* Defaults: enable = 0, tier_count = 1, cutoff = 0 (unset), bright = 0
 * (shadowed pixels are black, as in the original renderer). */
void sh_cfg_default(ShTruncCfg *cfg);

/*
 * Set one field.  `ty` must be the type returned by sh_field_type(f) and
 * `value` must point at an object of that type:
 *   SH_TY_INT   -> const int *
 *   SH_TY_FLOAT -> const float *
 * A wrong `ty` returns SH_ERR_TYPE with `err->field` naming the field; an
 * unknown field returns SH_ERR_FIELD; a NULL config/value returns
 * SH_ERR_NULL (also naming the field).  Per-field ranges are checked here:
 *   enable 0/1, tier_count 1..4, cutoff finite > 0, split_i finite >= 0,
 *   bright_i finite in [0,1].
 * Cross-field rules (splits strictly ascending and < cutoff, and (when
 * enabled) cutoff > 0) are checked by sh_cfg_validate().
 */
int sh_cfg_set(ShTruncCfg *cfg, ShField f, ShType ty, const void *value, ShError *err);

/* Validate the whole configuration.  With enable == 0 everything is ignored
 * and SH_OK is returned.  On failure `err->field` names the offending field. */
int sh_cfg_validate(const ShTruncCfg *cfg, ShError *err);

/*
 * Stage A.  Clip `segs` to the distance bands of `cfg` and write the pieces to
 * `out` (freed with sh_castset_free).  Requires cfg->enable != 0 and a valid
 * cfg; light position must be finite (radius/intensity are not used here).
 * Piece endpoints that are not original segment endpoints carry a sub-ulp
 * outward pad so the exact kept part is contained despite float rounding.
 */
int sh_truncate(ShCastSet *out, const ShLight *light, const ShSeg *segs, int nseg,
                const ShTruncCfg *cfg, ShError *err);

void sh_castset_free(ShCastSet *cs);

/*
 * Stage B.  Render using only the pieces in `cs`.  `cfg` must be the enabled
 * configuration the set was truncated with (tier_count/cutoff/splits must
 * match; bright may differ).  Shadows stop at `cutoff`; a pixel beyond it is
 * lit with the normal falloff.  `stats` is optional.
 */
int sh_render_truncated(ShadowMap *m, const ShLight *light, const ShCastSet *cs,
                        const ShTruncCfg *cfg, ShRenderStats *stats, ShError *err);

/*
 * Both stages in one call.  When cfg->enable == 0 the call is forwarded to
 * sh_render() unchanged and the result is byte-identical to the legacy
 * renderer.  When enabled, truncation stats are written to `cast_out` if it
 * is non-NULL (caller frees it); it is left empty in the disabled path.
 */
int sh_render_cast(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                   const ShTruncCfg *cfg, ShCastSet *cast_out, ShRenderStats *stats,
                   ShError *err);

#endif
