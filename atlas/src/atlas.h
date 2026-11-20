#ifndef ATLAS_H
#define ATLAS_H
typedef struct { int w, h; } AtlasRect;
typedef struct { int x, y; int placed; } AtlasPos;
typedef struct { int width; int used_px; int shelves; } AtlasStats;
int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats);
#endif
