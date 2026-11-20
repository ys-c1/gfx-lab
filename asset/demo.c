#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_index(const ApIndex *idx) {
    printf("version %u  entries %d  data %u bytes  blocks %d\n",
           idx->version, idx->count, idx->data_size, idx->block_count);
    for (int i = 0; i < idx->block_count; ++i) {
        const ApBlock *b = &idx->blocks[i];
        printf("  block %2d  raw [%6u, %6u)  stored %6u  file @ %6u  %s\n",
               i, b->raw_offset, b->raw_offset + b->raw_size, b->stored_size,
               b->file_offset, (b->flags & AP_BLOCK_FIRST_SCREEN) ? "first-screen" : "post-scene");
    }
}

int main(void) {
    /* Three resources: one small first-screen texture and two heavier
     * post-scene payloads. */
    unsigned char tex[1024], mesh[4096], audio[4096];
    for (int i = 0; i < 1024; ++i) tex[i] = (unsigned char)((i % 32) * 7);
    /* Quantized vertex/index data: repeated runs, like a real mesh page. */
    for (int i = 0; i < 4096; ++i) mesh[i] = (unsigned char)((i / 8) * 7 + i % 8);
    for (int i = 0; i < 4096; ++i) audio[i] = (unsigned char)((i % 64) * 4);

    ApItem items[3];
    memset(items, 0, sizeof(items));
    strcpy(items[0].name, "mesh.bin");  items[0].size = 4096; items[0].data = mesh;
    strcpy(items[1].name, "tex.bin");   items[1].size = 1024;  items[1].data = tex;  items[1].first_screen = 1;
    strcpy(items[2].name, "audio.bin"); items[2].size = 4096; items[2].data = audio;

    int err = -1;
    int rc = ap_pack("demo.gpak", items, 3, 1024, 1, &err);
    if (rc != 0) {
        printf("pack failed: %d (block %d)\n", rc, err);
        return 1;
    }
    ApIndex idx;
    if (ap_read("demo.gpak", &idx) != 0) return 1;
    dump_index(&idx);

    /* Entering the scene: inflate the first-screen blocks only. */
    unsigned char *first = NULL;
    unsigned int first_size = 0;
    int loaded = 0;
    if (ap_load_first_screen("demo.gpak", &idx, &first, &first_size, &loaded) != 0) return 1;
    printf("scene entry: %d of %d blocks, %u of %u bytes inflated\n",
           loaded, idx.block_count, first_size, idx.data_size);
    printf("  tex.bin %s\n", memcmp(first + idx.entries[1].offset, tex, 1024) == 0 ? "ok" : "MISMATCH");
    free(first);

    /* Whole package, the control path. */
    unsigned char *all = NULL;
    unsigned int all_size = 0;
    if (ap_load_all("demo.gpak", &idx, &all, &all_size, &loaded) != 0) return 1;
    printf("full load:   %d of %d blocks, %u bytes\n", loaded, idx.block_count, all_size);
    int ok = memcmp(all + idx.entries[0].offset, mesh, 4096) == 0 &&
             memcmp(all + idx.entries[2].offset, audio, 4096) == 0;
    printf("  mesh/audio %s\n", ok ? "ok" : "MISMATCH");
    free(all);
    ap_free(&idx);
    return ok ? 0 : 1;
}
