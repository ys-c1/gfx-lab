#define _POSIX_C_SOURCE 200809L
#include "tile.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){
    enum { W=12,H=8 };
    unsigned char *rgb=malloc(W*H*3);
    for(int i=0;i<W*H*3;++i) rgb[i]=(unsigned char)(i&0xff);
    int rc=tile_ppm_write(".tmp/p.ppm",rgb,W,H); printf("write rc=%d\n",rc);
    unsigned char *back=NULL; int rw=0,rh=0; char eb[128]={0};
    rc=tile_ppm_read(".tmp/p.ppm",&back,&rw,&rh,eb,sizeof eb);
    printf("read rc=%d w=%d h=%d err='%s' back=%p\n",rc,rw,rh,eb,(void*)back);
    return 0;
}
