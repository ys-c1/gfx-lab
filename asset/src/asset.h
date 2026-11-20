#ifndef ASSET_H
#define ASSET_H

/*
 * GPAK package layout.
 *
 * v1 (legacy, written by ap_write and by ap_pack(..., compress = 0)):
 *   0  "GPAK"
 *   4  u16 version = 1
 *   6  u16 entry_count
 *   8  entries[entry_count]: name[32], size u32, offset u32
 *      u32 data_size
 *      data
 *
 * v2 (chunked + compressed, written by ap_pack(..., compress = 1)):
 *   0  "GPAK"
 *   4  u16 version = 2
 *   6  u16 entry_count
 *   8  entries[entry_count]: name[32], size u32, offset u32
 *      u32 data_size                  uncompressed size of the logical stream
 *      u32 block_count
 *      blocks[block_count]:           block table (20 bytes each)
 *          u32 raw_offset             offset in the uncompressed stream
 *          u32 raw_size               uncompressed length (> 0)
 *          u32 stored_size            bytes on disk
 *          u32 file_offset            absolute offset of stored bytes
 *          u32 flags                  AP_BLOCK_*
 *      payload
 *
 * Entry offsets refer to the uncompressed logical stream in both versions.
 * With compression disabled the output bytes are identical to the old v1
 * writer, so existing readers keep working.
 */

#define AP_VERSION_LEGACY  1
#define AP_VERSION_CHUNKED 2

#define AP_BLOCK_FIRST_SCREEN 0x1u
#define AP_BLOCK_COMPRESSED   0x2u

/* Error codes.  Block-specific errors report the offending index via
 * *err_block (or the err_block argument of ap_read_ex). */
#define AP_E_ARG          -1
#define AP_E_IO           -2
#define AP_E_FORMAT       -3
#define AP_E_VERSION      -4
#define AP_E_NOTFOUND     -5
#define AP_E_BLOCK_EMPTY  -6  /* block raw_size == 0 */
#define AP_E_BLOCK_RANGE  -7  /* block offset/size outside the stream or file */
#define AP_E_BLOCK_GROW   -8  /* compressed block is not smaller than raw */
#define AP_E_BLOCK_ZLIB   -9  /* zlib failed to compress or inflate a block */
#define AP_E_CHUNK_ZERO  -10  /* chunk_size == 0 with compression enabled */

typedef struct {
    char name[32];
    unsigned int size;
    unsigned int offset;
} ApEntry;

typedef struct {
    unsigned int raw_offset;
    unsigned int raw_size;
    unsigned int stored_size;
    unsigned int file_offset;
    unsigned int flags;
} ApBlock;

typedef struct {
    ApEntry *entries;
    int count;
    unsigned int data_size;
    unsigned int version;
    ApBlock *blocks;
    int block_count;
} ApIndex;

/* Input item for ap_pack.  Items are laid out in array order. */
typedef struct {
    char name[32];
    unsigned int size;
    const unsigned char *data;
    int first_screen;
} ApItem;

/* v1 writer: unchanged on-disk bytes. */
int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size);

/* Read a package.  v1 packages yield version = 1 and no block table. */
int ap_read(const char *path, ApIndex *idx);
int ap_read_ex(const char *path, ApIndex *idx, int *err_block);

/* Extract one entry, decompressing only the block that contains it. */
int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size);
void ap_free(ApIndex *idx);

/*
 * Pack items into a package.
 *   compress = 0: write the legacy v1 layout (chunk_size is ignored).
 *   compress = 1: split items into blocks no larger than chunk_size, starting
 *                 a new block whenever the first_screen flag changes, and
 *                 compress every block.  A block that fails to shrink is an
 *                 error (AP_E_BLOCK_GROW), not a silent raw fallback.
 * Returns 0 on success, or a negative AP_E_* code; *err_block (when non-NULL)
 * receives the offending block index for block-specific errors, else -1.
 */
int ap_pack(const char *path, const ApItem *items, int n, unsigned int chunk_size,
            int compress, int *err_block);

/* Low-level v2 writer for an explicit block table.  Validates each block. */
int ap_write_chunked(const char *path, const ApEntry *entries, int n,
                     const ApBlock *blocks, int nblocks, unsigned int data_size,
                     const unsigned char *payload, unsigned int payload_size,
                     int *err_block);

/*
 * Decompress a subset of blocks into a freshly allocated buffer whose bytes
 * stay at their logical offsets; holes are zero-filled.
 *   ap_load_first_screen: only AP_BLOCK_FIRST_SCREEN blocks (entry into scene)
 *   ap_load_all:          every block (control / full install)
 * For v1 packages the whole payload is returned (there is nothing to split).
 * *out_size is the highest loaded end, so a first-screen load does not
 * allocate the full data_size.  *blocks_loaded receives the block count
 * inflated (0 for v1).
 */
int ap_load_first_screen(const char *path, const ApIndex *idx, unsigned char **out,
                         unsigned int *out_size, int *blocks_loaded);
int ap_load_all(const char *path, const ApIndex *idx, unsigned char **out,
                unsigned int *out_size, int *blocks_loaded);
#endif
