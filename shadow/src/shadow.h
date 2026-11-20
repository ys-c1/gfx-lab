#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* Dirty-block table for incremental re-render. */
typedef struct {
    int block;          /* block edge in pixels */
    int w, h;           /* map dimensions */
    int bw, bh;         /* block grid dimensions */
    unsigned char *tab; /* bw*bh flags, 1 = block needs recompute */
    float *acc;         /* internal: per-block drift bound since last recompute */
} ShBlocks;

/* Report for one incremental frame: block table + hit count. */
typedef struct {
    int bw, bh;                 /* block grid dimensions */
    int hit;                    /* number of dirty blocks recomputed */
    const unsigned char *tab;   /* bw*bh block table, valid until next sh_mark/sh_blocks_free */
} ShIncrStats;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* Incremental path. Return codes: 0 ok, -1 bad args, -2 bad radius/intensity,
 * -3 invalid block size, -4 non-finite light position (parameter named on stderr). */
int sh_blocks_init(ShBlocks *b, int w, int h, int block);
void sh_blocks_free(ShBlocks *b);
/* Phase 1: mark blocks affected by the light moving prev -> next (segs = occluders). */
int sh_mark(ShBlocks *b, const ShLight *prev, const ShLight *next, const ShSeg *segs, int nseg);
/* Phase 2: recompute only the marked blocks with the new light. */
int sh_render_blocks(ShadowMap *m, const ShBlocks *b, const ShLight *light, const ShSeg *segs, int nseg);
/* Both phases; fills stats with the block table and hit count when stats != NULL. */
int sh_render_incr(ShadowMap *m, ShBlocks *b, const ShLight *prev, const ShLight *next,
                   const ShSeg *segs, int nseg, ShIncrStats *stats);
#endif
