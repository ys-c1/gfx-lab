#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    enum { W = 64, H = 64 };
    unsigned char *src = (unsigned char*)malloc(W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y*W+x)*3;
            p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4); p[2] = 128;
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;
    printf("levels %d sizes %dx%d %dx%d\n", c.levels, c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1]);
    mip_free(&c);
    free(src);
    return 0;
}
