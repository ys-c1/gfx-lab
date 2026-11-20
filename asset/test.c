#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Bytes produced by the original v1 writer for the two-entry fixture below.
 * Compression-off packing must reproduce these exactly. */
static const unsigned char golden_v1[100] = {
    0x47,0x50,0x41,0x4b,0x01,0x00,0x02,0x00,0x61,0x2e,0x62,0x69,
    0x6e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x62,0x2e,0x62,0x69,0x6e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x02,0x03,0x04,
    0x05,0x06,0x07,0x08,
};

static long slurp(const char *path, unsigned char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = (unsigned char*)malloc(n ? (size_t)n : 1);
    if (*out && fread(*out, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*out); *out = NULL; return -1; }
    fclose(f);
    return n;
}

/* Overwrite a little-endian u32 in a file, used to forge broken block tables. */
static int patch32(const char *path, long off, unsigned int v) {
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    int rc = (fseek(f, off, SEEK_SET) == 0 && fwrite(b, 1, 4, f) == 4) ? 0 : -1;
    fclose(f);
    return rc;
}

static void test_legacy_bytes(void) {
    unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "a.bin"); e[0].size = 4; e[0].offset = 0;
    strcpy(e[1].name, "b.bin"); e[1].size = 4; e[1].offset = 4;
    CK(ap_write("build/t.gpak", e, 2, data, 8) == 0);

    unsigned char *bytes = NULL;
    long n = slurp("build/t.gpak", &bytes);
    CK(n == (long)sizeof(golden_v1));
    CK(n == (long)sizeof(golden_v1) && memcmp(bytes, golden_v1, sizeof(golden_v1)) == 0);
    free(bytes);

    ApIndex idx;
    CK(ap_read("build/t.gpak", &idx) == 0);
    CK(idx.count == 2 && idx.data_size == 8);
    CK(idx.version == AP_VERSION_LEGACY && idx.block_count == 0);
    CK(strcmp(idx.entries[1].name, "b.bin") == 0);
    ap_free(&idx);
    unsigned char *buf = NULL; unsigned int sz = 0;
    CK(ap_extract("build/t.gpak", "b.bin", &buf, &sz) == 0);
    CK(sz == 4 && buf[0] == 5);
    free(buf);
    CK(ap_extract("build/t.gpak", "zzz", &buf, &sz) == AP_E_NOTFOUND);
    CK(ap_write("build/bad.gpak", e, -1, data, 8) == AP_E_ARG);

    /* A package written by the original v1 writer stays readable. */
    FILE *lf = fopen("build/legacy.gpak", "wb");
    CK(lf != NULL);
    if (lf) { fwrite(golden_v1, 1, sizeof(golden_v1), lf); fclose(lf); }
    CK(ap_read("build/legacy.gpak", &idx) == 0);
    CK(idx.version == AP_VERSION_LEGACY && idx.count == 2);
    buf = NULL; sz = 0;
    CK(ap_extract("build/legacy.gpak", "a.bin", &buf, &sz) == 0);
    CK(sz == 4 && buf[3] == 4);
    free(buf);
    unsigned char *v1all = NULL; unsigned int v1sz = 0; int v1blocks = -1;
    CK(ap_load_first_screen("build/legacy.gpak", &idx, &v1all, &v1sz, &v1blocks) == 0);
    CK(v1sz == 8 && v1blocks == 0 && memcmp(v1all, data, 8) == 0);
    free(v1all);
    ap_free(&idx);
}

static void test_compress_off_matches(void) {
    unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ApItem it[2];
    memset(it, 0, sizeof(it));
    strcpy(it[0].name, "a.bin"); it[0].size = 4; it[0].data = data;
    strcpy(it[1].name, "b.bin"); it[1].size = 4; it[1].data = data + 4;
    CK(ap_pack("build/p0.gpak", it, 2, 0, 0, NULL) == 0);
    unsigned char *bytes = NULL;
    long n = slurp("build/p0.gpak", &bytes);
    CK(n == (long)sizeof(golden_v1) && memcmp(bytes, golden_v1, sizeof(golden_v1)) == 0);
    free(bytes);

    /* first_screen is layout-neutral when compression is off. */
    it[0].first_screen = 1;
    CK(ap_pack("build/p1.gpak", it, 2, 1, 0, NULL) == 0);
    bytes = NULL;
    n = slurp("build/p1.gpak", &bytes);
    CK(n == (long)sizeof(golden_v1) && memcmp(bytes, golden_v1, sizeof(golden_v1)) == 0);
    free(bytes);
}

static void test_chunking(void) {
    unsigned char src[4][100];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 100; ++j) src[i][j] = (unsigned char)('a' + (i + j) % 7);
    ApItem it[4];
    memset(it, 0, sizeof(it));
    for (int i = 0; i < 4; ++i) {
        snprintf(it[i].name, sizeof(it[i].name), "c%d.bin", i);
        it[i].size = 100;
        it[i].data = src[i];
    }
    int err = -1;
    CK(ap_pack("build/chunk.gpak", it, 4, 250, 1, &err) == 0);
    CK(err == -1);
    ApIndex idx;
    CK(ap_read_ex("build/chunk.gpak", &idx, &err) == 0);
    CK(idx.version == AP_VERSION_CHUNKED);
    CK(idx.data_size == 400);
    CK(idx.block_count == 2);
    CK(idx.blocks[0].raw_offset == 0 && idx.blocks[0].raw_size == 200);
    CK(idx.blocks[1].raw_offset == 200 && idx.blocks[1].raw_size == 200);
    for (int b = 0; b < idx.block_count; ++b) {
        CK(idx.blocks[b].flags & AP_BLOCK_COMPRESSED);
        CK(idx.blocks[b].stored_size < idx.blocks[b].raw_size);
        CK(idx.blocks[b].raw_offset + idx.blocks[b].raw_size <= idx.data_size);
    }
    for (int i = 0; i < idx.count; ++i) {
        unsigned char *buf = NULL; unsigned int sz = 0;
        CK(ap_extract("build/chunk.gpak", it[i].name, &buf, &sz) == 0);
        CK(sz == 100 && memcmp(buf, src[i], 100) == 0);
        free(buf);
    }
    unsigned char *all = NULL; unsigned int all_sz = 0; int loaded = -1;
    CK(ap_load_all("build/chunk.gpak", &idx, &all, &all_sz, &loaded) == 0);
    CK(loaded == 2 && all_sz == 400);
    CK(memcmp(all, src[0], 100) == 0 && memcmp(all + 100, src[1], 100) == 0 &&
       memcmp(all + 200, src[2], 100) == 0 && memcmp(all + 300, src[3], 100) == 0);
    free(all);
    ap_free(&idx);
}

static void test_first_screen(void) {
    unsigned char src[3][200];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 200; ++j) src[i][j] = (unsigned char)(i * 31 + j % 11);
    ApItem it[3];
    memset(it, 0, sizeof(it));
    for (int i = 0; i < 3; ++i) {
        snprintf(it[i].name, sizeof(it[i].name), "s%d.bin", i);
        it[i].size = 200;
        it[i].data = src[i];
    }
    it[2].first_screen = 1;  /* listed last, must still land in block 0 */
    CK(ap_pack("build/first.gpak", it, 3, 1024, 1, NULL) == 0);
    ApIndex idx;
    CK(ap_read("build/first.gpak", &idx) == 0);
    CK(idx.block_count == 2);
    CK(idx.blocks[0].flags & AP_BLOCK_FIRST_SCREEN);
    CK(!(idx.blocks[1].flags & AP_BLOCK_FIRST_SCREEN));
    CK(idx.blocks[0].raw_offset == 0 && idx.blocks[0].raw_size == 200);
    /* Entries keep array order but point into the reordered stream. */
    CK(strcmp(idx.entries[0].name, "s0.bin") == 0 && idx.entries[0].offset == 200);
    CK(strcmp(idx.entries[2].name, "s2.bin") == 0 && idx.entries[2].offset == 0);

    unsigned char *first = NULL; unsigned int first_sz = 0; int loaded = -1;
    CK(ap_load_first_screen("build/first.gpak", &idx, &first, &first_sz, &loaded) == 0);
    CK(loaded == 1);
    CK(first_sz == 200);                       /* only the first-screen prefix */
    CK(memcmp(first, src[2], 200) == 0);       /* no post-scene bytes inflated */
    free(first);

    unsigned char *all = NULL; unsigned int all_sz = 0;
    CK(ap_load_all("build/first.gpak", &idx, &all, &all_sz, &loaded) == 0);
    CK(loaded == 2 && all_sz == 600);
    CK(memcmp(all, src[2], 200) == 0);
    CK(memcmp(all + 200, src[0], 200) == 0 && memcmp(all + 400, src[1], 200) == 0);
    free(all);

    unsigned char *buf = NULL; unsigned int sz = 0;
    CK(ap_extract("build/first.gpak", "s1.bin", &buf, &sz) == 0);
    CK(sz == 200 && memcmp(buf, src[1], 200) == 0);
    free(buf);
    ap_free(&idx);
}

/* v2 layout: 8 header, 2*40 entries, 4 data_size, 4 block_count, blocks. */
#define TBL_OFF 96L

static void test_block_errors(void) {
    unsigned char src[2][300];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 300; ++j) src[i][j] = (unsigned char)((i * 7 + j) % 5);
    ApItem it[2];
    memset(it, 0, sizeof(it));
    strcpy(it[0].name, "x.bin"); it[0].size = 300; it[0].data = src[0]; it[0].first_screen = 1;
    strcpy(it[1].name, "y.bin"); it[1].size = 300; it[1].data = src[1];
    int err = 0;
    CK(ap_pack("build/err.gpak", it, 2, 1024, 1, &err) == 0);

    ApIndex good;
    CK(ap_read("build/err.gpak", &good) == 0);
    CK(good.block_count == 2);
    unsigned int raw1 = good.blocks[1].raw_size;
    unsigned int stored1 = good.blocks[1].stored_size;
    ap_free(&good);

    /* Block 1 with a zero raw size. */
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 4, 0) == 0);
    err = -1;
    CK(ap_read_ex("build/err.gpak", &good, &err) == AP_E_BLOCK_EMPTY);
    CK(err == 1);

    /* Block 1 offset past the end of the stream. */
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 4, raw1) == 0);
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 0, 600) == 0);
    err = -1;
    CK(ap_read_ex("build/err.gpak", &good, &err) == AP_E_BLOCK_RANGE);
    CK(err == 1);

    /* Block 1 stored bytes past the end of the file. */
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 0, 300) == 0);
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 12, 1u << 20) == 0);
    err = -1;
    CK(ap_read_ex("build/err.gpak", &good, &err) == AP_E_BLOCK_RANGE);
    CK(err == 1);

    /* Block 1 marked compressed but not smaller than raw. */
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 8, stored1) == 0);
    CK(patch32("build/err.gpak", TBL_OFF + 20 + 8, raw1) == 0);
    err = -1;
    CK(ap_read_ex("build/err.gpak", &good, &err) == AP_E_BLOCK_GROW);
    CK(err == 1);

    /* A corrupt block 0 must name block 0. */
    CK(patch32("build/err.gpak", TBL_OFF + 4, 0) == 0);
    err = -1;
    CK(ap_read_ex("build/err.gpak", &good, &err) == AP_E_BLOCK_EMPTY);
    CK(err == 0);

    /* chunk_size zero is rejected up front. */
    err = 0;
    CK(ap_pack("build/z.gpak", it, 2, 0, 1, &err) == AP_E_CHUNK_ZERO);

    /* Incompressible data must be reported, naming the offending block. */
    unsigned char noise[64];
    unsigned int seed = 12345;
    for (int i = 0; i < 64; ++i) { seed = seed * 1103515245u + 12345u; noise[i] = (unsigned char)(seed >> 16); }
    ApItem hard;
    memset(&hard, 0, sizeof(hard));
    strcpy(hard.name, "noise.bin"); hard.size = 64; hard.data = noise; hard.first_screen = 1;
    err = 0;
    CK(ap_pack("build/grow.gpak", &hard, 1, 1024, 1, &err) == AP_E_BLOCK_GROW);
    CK(err == 0);

    /* Low-level writer validates the same table. */
    ApEntry e[1];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "n.bin"); e[0].size = 4; e[0].offset = 0;
    /* 1 entry + 1 block => header is 8 + 40 + 8 + 20 = 76 bytes. */
    ApBlock b[1] = {{0, 4, 4, 76, AP_BLOCK_COMPRESSED}};
    unsigned char payload[4] = {1, 2, 3, 4};
    err = -1;
    CK(ap_write_chunked("build/w.gpak", e, 1, b, 1, 4, payload, 4, &err) == AP_E_BLOCK_GROW);
    CK(err == 0);
    b[0].raw_size = 0;
    err = -1;
    CK(ap_write_chunked("build/w.gpak", e, 1, b, 1, 4, payload, 4, &err) == AP_E_BLOCK_EMPTY);
    CK(err == 0);
}

int main(void) {
    test_legacy_bytes();
    test_compress_off_matches();
    test_chunking();
    test_first_screen();
    test_block_errors();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
