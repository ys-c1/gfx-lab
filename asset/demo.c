#define _POSIX_C_SOURCE 200809L
#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Demo: what one bad block costs, and what the three stages buy.
 *
 *   1 MiB  and 16 MiB payloads, 4 KiB blocks, one texture per block.
 *   3 blocks are damaged: 2 recoverable from redundant copies, 1 with every
 *   copy destroyed.
 *
 * Reported per tier: missing textures, repair time, extra bytes.  The
 * "hand" lines are computed from the layout by hand (block count, per-block
 * arithmetic) and checked against the file on disk. */

#define BLOCK 4096u
#define COPIES 2
#define ONE_MIB (1024u * 1024u)
#define SIXTEEN_MIB (16u * 1024u * 1024u)

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { ++failures; printf("  !! %s\n", msg); } } while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* Overwrite one byte at an absolute offset with a value that is not what is
 * already there.  Idempotent, so re-running an iteration on an already
 * damaged byte does not silently heal it. */
static void corrupt(const char *path, long off) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    unsigned char b = 0;
    if (fread(&b, 1, 1, f) != 1) b = 0;
    b = (unsigned char)(b == 0xA5 ? 0x5A : 0xA5);
    fseek(f, off, SEEK_SET);
    if (fwrite(&b, 1, 1, f) != 1) { /* ignore */ }
    fclose(f);
}

static void die(const char *what, int rc) {
    printf("  !! %s returned %d\n", what, rc);
    ++failures;
}

/* Build the pack for one tier.  One entry per block, so the mapping from a
 * bad block to a missing texture is exact and hand-checkable. */
static unsigned char *build_pack(const char *path, unsigned int payload,
                                 unsigned int *entries_out, unsigned int *blocks_out) {
    unsigned int blocks = payload / BLOCK;
    unsigned int hdr = 8 + blocks * 40 + 4;
    unsigned char *data = (unsigned char*)malloc(payload);
    ApEntry *ent = (ApEntry*)calloc(blocks, sizeof(ApEntry));
    if (!data || !ent) { free(data); free(ent); return NULL; }
    for (unsigned int i = 0; i < payload; ++i) data[i] = (unsigned char)(i * 31u + (i >> 8));
    for (unsigned int b = 0; b < blocks; ++b) {
        snprintf(ent[b].name, sizeof(ent[b].name), "tex/%04u.bin", b);
        ent[b].offset = b * BLOCK;
        ent[b].size = BLOCK;
    }
    int rc = ap_write_checked(path, ent, (int)blocks, data, payload, BLOCK, COPIES);
    if (rc != 0) die("ap_write_checked", rc);
    free(ent);
    *entries_out = blocks;
    *blocks_out = blocks;
    (void)hdr;
    return data;
}

static void run_tier(const char *label, unsigned int payload, int iters) {
    char path[128];
    snprintf(path, sizeof(path), "build/bench_%u.gpak", payload);
    unsigned int nent = 0, nblk = 0;
    unsigned char *data = build_pack(path, payload, &nent, &nblk);
    if (!data) return;

    /* ---- hand-computed baseline, per block ---------------------------- */
    unsigned int checksum_bytes = nblk * 4u;
    unsigned int copy_bytes = COPIES * payload;
    unsigned int extra_hand = 12u + checksum_bytes + copy_bytes; /* layout + sums + copies */
    long file_hand = (long)(8u + nent * 40u + 4u) + (long)payload + (long)extra_hand;
    long file_real = file_size(path);

    /* Byte offsets from the layout, also by hand. */
    long data_at = 8L + (long)nent * 40L + 4L + 12L + (long)checksum_bytes;
    long copy_at = data_at + (long)payload;

    printf("\n== %s payload: %u bytes, %u blocks, block %u B, %d copies ==\n",
           label, payload, nblk, BLOCK, COPIES);
    printf("   hand: extra = 12 + %u*4 + %d*%u = %u bytes\n",
           nblk, COPIES, payload, extra_hand);
    printf("   hand: each block costs 4 checksum bytes + %d*%u copy bytes = %u\n",
           COPIES, BLOCK, 4u + COPIES * BLOCK);
    printf("   hand: file = header(%u) + data(%u) + extra(%u) = %ld bytes\n",
           8u + nent * 40u + 4u, payload, extra_hand, file_hand);
    printf("   disk: file = %ld bytes  -> %s\n", file_real,
           file_real == file_hand ? "matches the hand count" : "MISMATCH");

    /* ---- damage: block 10 and 200 recoverable, block 50 not ---------- */
    unsigned int b_ok1 = 10, b_ok2 = nblk / 2, b_lost = 50;
    if (b_ok2 == b_ok1 || b_ok2 == b_lost) b_ok2 = nblk - 1;
    corrupt(path, data_at + (long)b_ok1 * BLOCK + 7);
    corrupt(path, data_at + (long)b_ok2 * BLOCK + 11);
    corrupt(path, data_at + (long)b_lost * BLOCK + 5);
    for (int c = 0; c < COPIES; ++c) /* every copy of the lost block, too */
        corrupt(path, copy_at + (long)c * payload + (long)b_lost * BLOCK + 5);

    /* Stage 2: locate. */
    double t0 = now_ms();
    ApReport scan;
    ap_report_init(&scan);
    int rc = ap_check(path, &scan);
    double scan_ms = now_ms() - t0;
    if (rc != 0) die("ap_check", rc);
    printf("   locate: bad primary blocks = %d, bad copy blocks = %d, "
           "affected textures = %d\n",
           scan.bad_primary, scan.bad_copies, scan.affected_entries);
    printf("   locate: blocks %d and %d repairable, block %d not "
           "(first affected texture: %s)\n",
           b_ok1, b_ok2, b_lost,
           scan.issues[scan.issue_count - 1].first_name);

    /* Plain load (repair off) still hands back the damaged bytes silently,
     * exactly as the historic loader did. */
    unsigned char *plain = NULL;
    unsigned int plain_sz = 0;
    char missing_name[32];
    snprintf(missing_name, sizeof(missing_name), "tex/%04u.bin", b_ok1);
    rc = ap_extract(path, missing_name, &plain, &plain_sz);
    int plain_quiet = rc == 0 &&
        memcmp(plain, data + (size_t)b_ok1 * BLOCK, BLOCK) != 0;
    free(plain);
    unsigned int scan_missing_off = (unsigned int)scan.affected_entries;
    ap_report_free(&scan);

    /* Stage 3: repair in place, timed over several fresh iterations. */
    double total_ms = 0.0;
    int repaired = 0, missing = 0;
    for (int it = 0; it < iters; ++it) {
        corrupt(path, data_at + (long)b_ok1 * BLOCK + 7);
        corrupt(path, data_at + (long)b_ok2 * BLOCK + 11);
        corrupt(path, data_at + (long)b_lost * BLOCK + 5);
        for (int c = 0; c < COPIES; ++c)
            corrupt(path, copy_at + (long)c * payload + (long)b_lost * BLOCK + 5);
        ApReport rep;
        ap_report_init(&rep);
        double s = now_ms();
        rc = ap_repair(path, &rep);
        total_ms += now_ms() - s;
        if (it == 0) {
            /* One lost texture: everything else comes back. */
            missing = rep.missing_entries;
            repaired = rep.bad_primary - rep.unrecoverable;
        }
        CHECK(rc == AP_E_COPY, "ap_repair should flag the unrecoverable block");
        if (it == 0) CHECK(rep.fault.block == (int)b_lost, "fault names the lost block");
        CHECK(rep.repaired == 2, "two blocks restored per iteration");
        ap_report_free(&rep);
    }
    double avg_ms = total_ms / (double)iters;

    unsigned int missing_hand = 1;             /* one block with no good copy */
    unsigned int repaired_hand = 2;            /* the other two blocks */
    printf("   repair: restored %d blocks (hand: %u), missing textures %d (hand: %u)\n",
           repaired, repaired_hand, missing, missing_hand);
    printf("   repair: scan %.2f ms, scan+repair %.2f ms (avg of %d), "
           "%.1f ms per MiB scanned\n",
           scan_ms, avg_ms, iters, scan_ms / ((double)payload / (1024.0 * 1024.0)));
    printf("   repair: repair off -> no error and the wrong byte survives: %s\n",
           plain_quiet ? "yes, same as the old loader" : "NO (unexpected)");

    /* What the copies cost versus what they save: with repair off you lose
     * every texture touching a bad block; with it on only the unrecoverable
     * one. */
    unsigned int missing_off = scan_missing_off;
    unsigned int missing_on = (unsigned int)missing;
    double extra_pct = 100.0 * (double)extra_hand / (double)payload;
    printf("   cost: +%u bytes (%.2f%% of payload), missing textures %u -> %u\n",
           extra_hand, extra_pct, missing_off, missing_on);

    CHECK(file_size(path) == file_real, "repair must not change the file size");
    CHECK(repaired == (int)repaired_hand, "repaired count vs hand baseline");
    CHECK(missing == (int)missing_hand, "missing count vs hand baseline");
    CHECK(plain_quiet, "plain load still returns raw bytes when repair is off");

    free(data);
    remove(path);
}

/* The historical demo artifact: a tiny v1 pack, still byte-compatible. */
static void write_demo_artifact(void) {
    unsigned char data[16];
    for (int i = 0; i < 16; ++i) data[i] = (unsigned char)(i * 3);
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "tex.bin"); e[0].size = 8; e[0].offset = 0;
    strcpy(e[1].name, "idx.bin"); e[1].size = 8; e[1].offset = 8;
    if (ap_write("demo.gpak", e, 2, data, 16) != 0) die("ap_write(demo)", -1);
    ApIndex idx;
    if (ap_read("demo.gpak", &idx) != 0) { die("ap_read(demo)", -1); return; }
    printf("demo.gpak: entries %d data %u\n", idx.count, idx.data_size);
    for (int i = 0; i < idx.count; ++i)
        printf("  %s %u\n", idx.entries[i].name, idx.entries[i].size);
    ap_free(&idx);
}

int main(void) {
    mkdir("build", 0777);
    write_demo_artifact();

    run_tier("1 MiB", ONE_MIB, 20);
    run_tier("16 MiB", SIXTEEN_MIB, 5);

    printf("\n%s\n", failures ? "BENCHMARK FAILED" : "benchmark baselines all match");
    return failures ? 1 : 0;
}
