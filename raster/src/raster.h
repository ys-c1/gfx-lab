#ifndef RASTER_H
#define RASTER_H

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;

/* ------------------------------------------------------------------ *
 * Stencil buffer
 *
 * Rendering with a stencil is split into three explicit phases:
 *
 *   1. rt_stencil_write  - stamp stencil values through a shape (circle)
 *   2. rt_stencil_test   - compare every pixel's value against a reference
 *                          through a bit mask, and record pass/fail
 *   3. rt_stencil_shade  - colour exactly the pixels that passed
 *
 * rt_init leaves the stencil disabled, so the plain rt_triangle path and
 * every byte it produces are unchanged.  While disabled the three phase
 * calls are successful no-ops and touch neither rgb nor z.
 * ------------------------------------------------------------------ */

typedef enum {
    RT_STENCIL_FUNC_NEVER = 0,
    RT_STENCIL_FUNC_LESS,
    RT_STENCIL_FUNC_EQUAL,
    RT_STENCIL_FUNC_LEQUAL,
    RT_STENCIL_FUNC_GREATER,
    RT_STENCIL_FUNC_NOTEQUAL,
    RT_STENCIL_FUNC_GEQUAL,
    RT_STENCIL_FUNC_ALWAYS,
    RT_STENCIL_FUNC_COUNT
} RTStencilFunc;

/* Error codes returned negated, e.g. RT_STENCIL_ERR_BAD_FUNC -> -5. */
typedef enum {
    RT_STENCIL_ERR_NONE = 0,
    RT_STENCIL_ERR_NULL,       /* NULL pointer argument */
    RT_STENCIL_ERR_GEOMETRY,   /* non-finite centre or radius <= 0 */
    RT_STENCIL_ERR_BAD_FUNC,   /* func outside [0, RT_STENCIL_FUNC_COUNT) */
    RT_STENCIL_ERR_BAD_MASK,   /* zero write/read mask */
    RT_STENCIL_ERR_BAD_VALUE,  /* value has bits outside the relevant mask */
    RT_STENCIL_ERR_STATE,      /* phase order violated */
    RT_STENCIL_ERR_INDEX,      /* pixel coordinate outside the target */
    RT_STENCIL_ERR_RANGE,      /* other out-of-range argument (n, bit, z) */
    RT_STENCIL_ERR_COUNT
} RTStencilError;

/* Filled by every rejecting call.  `param` names the offending argument,
 * `index` is the offending bit for mask/value errors, and `x`/`y` identify
 * the pixel for index errors.  Unused fields are -1 (or NULL for param). */
typedef struct {
    RTStencilError code;
    const char *param;
    int index;
    int x, y;
    double value;
} RTStencilErrorInfo;

typedef struct {
    double cx, cy, radius;     /* circle; a pixel is written when its
                                * centre lies inside (<= radius) */
    unsigned char value;       /* value to write */
    unsigned char write_mask;  /* bits that may be changed; must be nonzero */
} RTStencilShape;

typedef struct {
    RTStencilFunc func;
    unsigned char ref;         /* reference value */
    unsigned char read_mask;   /* bits compared; must be nonzero */
} RTStencilTest;

typedef struct {
    double z;                  /* depth stored for shaded pixels */
    unsigned char rgb[3];      /* colour written to shaded pixels */
} RTStencilShade;

/* Per-phase report.  `pixels` is always w*h; the other counters are filled
 * for the phase just run.  `hist` and min/max describe the stencil buffer
 * after the phase.  `elapsed_ms` times the pixel kernel, not the report. */
typedef struct {
    int pixels;
    int covered;               /* write: pixel centres inside the shape */
    int written;               /* write: pixels whose value changed */
    int tested;                /* test: pixels visited */
    int passes;                /* test: pixels that satisfied the function */
    int shaded;                /* shade: pixels coloured */
    int min_value;
    int max_value;
    unsigned int hist[256];
    double elapsed_ms;
} RTStencilStats;

enum {
    RT_STENCIL_PHASE_NONE = 0,
    RT_STENCIL_PHASE_WRITE = 1,
    RT_STENCIL_PHASE_TEST = 2,
    RT_STENCIL_PHASE_SHADE = 3
};

typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    unsigned char *stencil;    /* one byte per pixel */
    unsigned char *pass;       /* 1 when the last test passed, else 0 */
    int stencil_enabled;
    int phase;                 /* RT_STENCIL_PHASE_* */
} RTarget;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_write_ppm(const RTarget *t, const char *path);

int rt_stencil_enable(RTarget *t, int enabled);
int rt_stencil_enabled(const RTarget *t);

/* Reset every stencil value to `value` and forget the phase/pass state.
 * No-op while disabled. */
int rt_stencil_clear(RTarget *t, unsigned char value, RTStencilErrorInfo *err);

/* Phase 1: stamp values through a circle. */
int rt_stencil_write(RTarget *t, const RTStencilShape *shape,
                     RTStencilStats *stats, RTStencilErrorInfo *err);

/* Phase 2: test all pixels.  Legal after WRITE or TEST; re-running the test
 * replaces the previous pass set. */
int rt_stencil_test(RTarget *t, const RTStencilTest *test,
                    RTStencilStats *stats, RTStencilErrorInfo *err);

/* Phase 3: colour the pixels that passed.  Legal only once after TEST. */
int rt_stencil_shade(RTarget *t, const RTStencilShade *shade,
                     RTStencilStats *stats, RTStencilErrorInfo *err);

/* Direct access.  Out-of-range coordinates are rejected with
 * RT_STENCIL_ERR_INDEX, with x/y naming the offending pixel. */
int rt_stencil_get(const RTarget *t, int x, int y, unsigned char *out,
                   RTStencilErrorInfo *err);
int rt_stencil_pass_at(const RTarget *t, int x, int y, unsigned char *out,
                       RTStencilErrorInfo *err);

/* Bitmask retrieval.  Bits are packed row-major, LSB first: pixel i occupies
 * bit (i & 7) of byte (i >> 3).  out_bytes must be (w*h + 7)/8.
 *
 *   export_pass_bits   one bit per pixel, 1 when the last test passed
 *   export_value_bits  one bit per pixel, taken from stencil bit `bit`
 */
int rt_stencil_export_values(const RTarget *t, unsigned char *out, int n,
                             RTStencilErrorInfo *err);
int rt_stencil_export_pass_bits(const RTarget *t, unsigned char *out,
                                int out_bytes, RTStencilErrorInfo *err);
int rt_stencil_export_value_bits(const RTarget *t, int bit,
                                 unsigned char *out, int out_bytes,
                                 RTStencilErrorInfo *err);

const char *rt_stencil_error_name(RTStencilError e);
const char *rt_stencil_phase_name(int phase);

#endif
