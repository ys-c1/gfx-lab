#include "atlas.h"
#include <stdio.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    AtlasRect r[4] = {{16,16},{32,8},{8,8},{64,4}};
    AtlasPos p[4];
    AtlasStats s;
    CK(atlas_pack(r, 4, 64, p, &s) == 0);
    CK(p[0].x == 0 && p[0].y == 0);
    CK(s.shelves >= 2);
    CK(s.used_px == 16*16 + 32*8 + 8*8 + 64*4);
    AtlasRect bad[1] = {{128, 4}};
    CK(atlas_pack(bad, 1, 64, p, &s) == -2);
    CK(atlas_pack(r, 0, 64, p, &s) == -1);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
