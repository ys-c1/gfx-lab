#ifndef MIP_H
#define MIP_H
typedef struct {
    int levels;
    int w0, h0;
    unsigned char **pix;
    int *w, *h;
} MipChain;
int mip_build(MipChain *c, const unsigned char *src, int w, int h);
void mip_free(MipChain *c);
int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]);
#endif
