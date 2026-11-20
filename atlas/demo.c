#include "atlas.h"
#include <stdio.h>
int main(void) {
    AtlasRect r[8];
    for (int i = 0; i < 8; ++i) { r[i].w = 8 + i * 4; r[i].h = 8 + (i % 3) * 8; }
    AtlasPos p[8];
    AtlasStats s;
    if (atlas_pack(r, 8, 128, p, &s) != 0) return 1;
    printf("shelves=%d used=%d\n", s.shelves, s.used_px);
    for (int i = 0; i < 8; ++i) printf("  %d: %d,%d\n", i, p[i].x, p[i].y);
    return 0;
}
