#include "mip.h"
#include <stdlib.h>

int mip_build(MipChain *c, const unsigned char *src, int w, int h) {
    if (!c || !src || w <= 0 || h <= 0) return -1;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) { mip_free(c); return -1; }
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) { mip_free(c); return -1; }
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) { mip_free(c); return -1; }
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x*2 < pw ? x*2 : pw-1, x1 = (x*2+1) < pw ? x*2+1 : pw-1;
                    int y0 = y*2 < ph ? y*2 : ph-1, y1 = (y*2+1) < ph ? y*2+1 : ph-1;
                    int s = c->pix[l-1][(y0*pw+x0)*3+ch] + c->pix[l-1][(y0*pw+x1)*3+ch]
                          + c->pix[l-1][(y1*pw+x0)*3+ch] + c->pix[l-1][(y1*pw+x1)*3+ch];
                    c->pix[l][(y*nw+x)*3+ch] = (unsigned char)(s / 4);
                }
    }
    return 0;
}

void mip_free(MipChain *c) {
    if (!c) return;
    for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL; c->levels = 0;
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}
