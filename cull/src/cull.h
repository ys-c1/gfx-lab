#ifndef CULL_H
#define CULL_H
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;

/* ---- legacy one-shot cull (behavior kept unchanged) ---- */
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ---- animated viewport culling ---- */

/* error codes */
#define CULL_OK         0
#define CULL_E_ARG     -1  /* null pointer or n < 0                      */
#define CULL_E_VIEW    -2  /* view width/height <= 0                     */
#define CULL_E_EMPTY   -3  /* n == 0: no boxes, cull result undefined    */
#define CULL_E_T       -4  /* interpolation coefficient t outside [0,1]  */
#define CULL_E_STATE   -5  /* anim used without cull_anim_init           */
#define CULL_E_NOMEM   -6  /* allocation failed                          */
#define CULL_E_IO      -7  /* csv export failed                          */

/* flag: rebuild the full result over all n boxes on every frame,
   reproducing cull_run exactly (legacy behavior, no incremental path) */
#define CULL_ANIM_FULL_REBUILD 1

/* one frame's report. `visible` points into the CullAnim and stays valid
   until the next cull_anim_step / cull_anim_free call. */
typedef struct {
    int frame;
    double t;              /* interpolation coefficient actually used   */
    CullView view;         /* phase-1 output: interpolated view         */
    int candidates;        /* boxes that may change during the anim     */
    int rebuilt;           /* 1 if this frame did a full rebuild        */
    int added;             /* newly visible vs previous frame           */
    int removed;           /* no longer visible vs previous frame       */
    int unchanged;         /* visible both before and after             */
    int visible_count;
    const int *visible;    /* sorted ascending visible ids              */
} CullStepInfo;

/* owned snapshot of one frame, used for trajectory export */
typedef struct {
    int frame;
    double t;
    double vx, vy, vw, vh;
    int candidates;
    int rebuilt;
    int added, removed, unchanged;
    int visible_count;
    int *visible_ids;
} CullFrameRecord;

typedef struct {
    const CullBox *boxes;
    int n;
    CullView v0, v1, cur;
    int flags;
    int *cand;             /* ascending ids overlapping union(v0,v1)    */
    int ncand;
    int *visible; int nvis;
    int *prev;    int nprev;
    int frame;
    int rebuilds;
    CullFrameRecord *hist;
    int nhist, hcap;
} CullAnim;

const char *cull_strerror(int code);

/* phase 1: interpolate an intermediate view between front/back keyframes */
CullView cull_view_lerp(CullView a, CullView b, double t);

/* Initializes the anim and immediately runs frame 0 at t=0.
   Returns a CULL_E_* code; on success *first (when non-NULL) holds the
   frame-0 report. n==0 is rejected with CULL_E_EMPTY. */
int cull_anim_init(CullAnim *a, const CullBox *boxes, int n,
                   CullView v0, CullView v1, int flags,
                   CullStepInfo *first);

/* phase 2: advance to coefficient t in [0,1] and update only the
   candidate subset; fills *info when non-NULL. */
int cull_anim_step(CullAnim *a, double t, CullStepInfo *info);

void cull_anim_free(CullAnim *a);

/* export the full interpolated trajectory / the box table as csv */
int cull_anim_export_csv(const CullAnim *a, const char *path);
int cull_anim_export_boxes(const CullBox *boxes, int n, const char *path);
#endif
