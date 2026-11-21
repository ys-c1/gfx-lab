#include "tile.h"
#include <stdio.h>
int main(void){
  TileOutline o; int r;
  r=tile_outline_build(&o,-1,8,4,4); printf("w负: rc=%d %s\n",r,o.errbuf);
  r=tile_outline_build(&o,8,-1,4,4); printf("h负: rc=%d %s\n",r,o.errbuf);
  r=tile_outline_build(&o,8,8,-4,4); printf("tile_w负: rc=%d %s\n",r,o.errbuf);
  r=tile_outline_build(&o,8,8,4,-4); printf("tile_h负: rc=%d %s\n",r,o.errbuf);
  return 0;
}
