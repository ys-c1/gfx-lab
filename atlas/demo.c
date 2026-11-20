#include "atlas.h"
#include <stdio.h>

int main(void) {
    AtlasRect r[8];
    for (int i = 0; i < 8; ++i) { r[i].w = 8 + i * 4; r[i].h = 8 + (i % 3) * 8; }
    AtlasPos p[8];
    AtlasStats s;

    if (atlas_pack(r, 8, 128, p, &s) != 0) return 1;
    printf("baseline:        shelves=%d used=%d height=%d\n",
           s.shelves, s.used_px, s.height);
    for (int i = 0; i < 8; ++i)
        printf("  %d: %d,%d%s\n", i, p[i].x, p[i].y,
               p[i].rotated ? " rot" : "");

    const struct { const char *name; unsigned flags; } modes[] = {
        {"height sort",    ATLAS_SORT_HEIGHT},
        {"rotate",         ATLAS_ALLOW_ROTATE},
        {"sort + rotate",  ATLAS_SORT_HEIGHT | ATLAS_ALLOW_ROTATE},
    };
    for (int m = 0; m < 3; ++m) {
        if (atlas_pack_ex(r, 8, 128, modes[m].flags, NULL, p, &s, NULL) != 0)
            return 1;
        printf("%-16s shelves=%d used=%d height=%d\n",
               modes[m].name, s.shelves, s.used_px, s.height);
        for (int i = 0; i < 8; ++i)
            printf("  %d: %d,%d%s\n", i, p[i].x, p[i].y,
                   p[i].rotated ? " rot" : "");
    }
    return 0;
}
