#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* Test packs live under build/, never in /tmp. */
static const char *P(const char *name) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "build/%s", name);
    return buf;
}
static void ensure_out(void) { mkdir("build", 0777); }

static long file_len(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void poke(const char *path, long off, unsigned char v) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    if (fwrite(&v, 1, 1, f) != 1) { /* ignore */ }
    fclose(f);
}

static unsigned int rd32(const char *path, long off) {
    unsigned int v = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, off, SEEK_SET);
    if (fread(&v, 4, 1, f) != 1) v = 0;
    fclose(f);
    return v;
}

static void truncate_at(const char *path, long n) {
    FILE *in = fopen(path, "rb");
    if (!in) return;
    unsigned char *buf = (unsigned char*)malloc((size_t)(n > 0 ? n : 1));
    long got = (long)fread(buf, 1, (size_t)(n > 0 ? n : 0), in);
    fclose(in);
    FILE *out = fopen(path, "wb");
    if (out) { fwrite(buf, 1, (size_t)got, out); fclose(out); }
    free(buf);
}

/* ------------------------------------------------------------- fixtures */

#define BS 64u
#define NBLK 5u /* 260 bytes / 64 */
static unsigned char DATA[260];
static ApEntry ENT[3];

static void make_fixture(void) {
    for (unsigned int i = 0; i < sizeof(DATA); ++i) DATA[i] = (unsigned char)(i * 7u + 3u);
    memset(ENT, 0, sizeof(ENT));
    /* a.bin [0,50)   -> block 0 only
     * b.bin [50,80)  -> blocks 0 and 1
     * z.bin [80,260) -> blocks 1..4                                   */
    strcpy(ENT[0].name, "tex/a.bin"); ENT[0].offset = 0;  ENT[0].size = 50;
    strcpy(ENT[1].name, "tex/b.bin"); ENT[1].offset = 50; ENT[1].size = 30;
    strcpy(ENT[2].name, "tex/z.bin"); ENT[2].offset = 80; ENT[2].size = 180;
}

static int matches(const unsigned char *got, unsigned int sz, const char *name) {
    for (int i = 0; i < 3; ++i) {
        if (strcmp(ENT[i].name, name) != 0) continue;
        return sz == ENT[i].size &&
               memcmp(got, DATA + ENT[i].offset, ENT[i].size) == 0;
    }
    return 0;
}

/* Byte layout of a v2 pack with the fixture above:
 *   [0,8)      magic, ver, count
 *   [8,128)    entries
 *   [128,132)  data_size
 *   [132,144)  block_count, block_size, copies
 *   [144,164)  checksums
 *   [164,424)  data (blocks 0..3 of 64 bytes, block 4 of 4)
 *   [424,...)  redundant copies                                      */
#define HDR_V2  128L
#define DATA_AT 164L
#define SZ_V2(copies) (164L + 260L + (long)(copies) * 260L)

static void test_legacy(void) {
    make_fixture();
    CK(ap_write(P("legacy.gpak"), ENT, 3, DATA, sizeof(DATA)) == 0);
    ApIndex idx;
    CK(ap_read(P("legacy.gpak"), &idx) == 0);
    CK(idx.version == AP_VERSION_LEGACY);
    CK(idx.count == 3 && idx.data_size == 260 && idx.block_count == 0);
    CK(strcmp(idx.entries[2].name, "tex/z.bin") == 0);
    ap_free(&idx);

    /* v1 bytes are frozen: same header, same total length, same data. */
    CK(file_len(P("legacy.gpak")) == 8 + 3 * 40 + 4 + 260);
    CK(rd32(P("legacy.gpak"), 0) == 0x4b415047u /* "GPAK" */);
    CK(rd32(P("legacy.gpak"), 4) == (3u << 16) + (unsigned)AP_VERSION_LEGACY);
    CK(rd32(P("legacy.gpak"), 8 + 3 * 40) == 260);

    unsigned char *buf = NULL;
    unsigned int sz = 0;
    CK(ap_extract(P("legacy.gpak"), "tex/z.bin", &buf, &sz) == 0);
    CK(matches(buf, sz, "tex/z.bin"));
    free(buf);
    CK(ap_extract(P("legacy.gpak"), "nope", &buf, &sz) == AP_E_NOTFOUND);
    CK(ap_write(P("bad.gpak"), ENT, -1, DATA, sizeof(DATA)) == AP_E_ARG);
}

static void test_checked_roundtrip(void) {
    make_fixture();
    CK(ap_write_checked(P("chk.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 2) == 0);
    CK(file_len(P("chk.gpak")) == SZ_V2(2));

    ApIndex idx;
    CK(ap_read(P("chk.gpak"), &idx) == 0);
    CK(idx.version == AP_VERSION_CHECKED && idx.block_count == NBLK);
    ap_free(&idx);

    /* Healthy pack: plain, verify and repair loads agree byte for byte. */
    for (int mode = 0; mode < 3; ++mode) {
        unsigned int flags = mode == 0 ? AP_LOAD_PLAIN
                                     : mode == 1 ? AP_LOAD_VERIFY : AP_LOAD_REPAIR;
        unsigned char *buf = NULL;
        unsigned int sz = 0;
        ApReport rep;
        ap_report_init(&rep);
        CK(ap_extract_ex(P("chk.gpak"), "tex/z.bin", &buf, &sz, flags, &rep) == 0);
        CK(matches(buf, sz, "tex/z.bin"));
        CK(rep.repaired == 0 && rep.fault.code == 0);
        free(buf);
        ap_report_free(&rep);
    }

    ApReport rep;
    ap_report_init(&rep);
    CK(ap_check(P("chk.gpak"), &rep) == 0);
    CK(rep.issue_count == 0 && rep.bad_primary == 0 && rep.bad_copies == 0);
    CK(rep.block_size == (int)BS && rep.block_count == (int)NBLK && rep.copies == 2);
    ap_report_free(&rep);

    /* Damage primary block 1 (bytes 64..128): tail of b.bin, head of z.bin. */
    poke(P("chk.gpak"), DATA_AT + 70, (unsigned char)(DATA[70] ^ 0xff));

    ap_report_init(&rep);
    CK(ap_check(P("chk.gpak"), &rep) == 0); /* scan completes, reports damage */
    CK(rep.bad_primary == 1 && rep.issue_count == 1);
    CK(rep.issues[0].block == 1);
    CK(rep.issues[0].entries == 2);
    CK(rep.issues[0].first_entry == 1);
    CK(strcmp(rep.issues[0].first_name, "tex/b.bin") == 0);
    CK(rep.issues[0].copies_ok == 2 && rep.issues[0].copies_bad == 0);
    CK(rep.affected_entries == 2 && rep.missing_entries == 0);
    ap_report_free(&rep);

    unsigned char *buf = NULL;
    unsigned int sz = 0;

    /* Verify only: refuse, and point at block + entry. */
    ap_report_init(&rep);
    CK(ap_extract_ex(P("chk.gpak"), "tex/b.bin", &buf, &sz, AP_LOAD_VERIFY, &rep) == AP_E_CORRUPT);
    CK(rep.fault.block == 1 && rep.fault.entry == 1);
    CK(strcmp(rep.fault.name, "tex/b.bin") == 0);
    CK(buf == NULL);
    ap_report_free(&rep);

    /* Repair on load: correct bytes. */
    ap_report_init(&rep);
    CK(ap_extract_ex(P("chk.gpak"), "tex/b.bin", &buf, &sz, AP_LOAD_REPAIR, &rep) == 0);
    CK(matches(buf, sz, "tex/b.bin"));
    CK(rep.repaired == 1);
    free(buf);
    buf = NULL;
    ap_report_free(&rep);
    ap_report_init(&rep);
    CK(ap_extract_ex(P("chk.gpak"), "tex/z.bin", &buf, &sz, AP_LOAD_REPAIR, &rep) == 0);
    CK(matches(buf, sz, "tex/z.bin")); /* entry touching the bad block too */
    free(buf);
    buf = NULL;
    ap_report_free(&rep);

    /* Plain load with repair off returns exactly what the old loader would:
     * the damaged byte, no checks and no substitution. */
    CK(ap_extract(P("chk.gpak"), "tex/b.bin", &buf, &sz) == 0);
    CK(sz == 30 && buf[20] == (unsigned char)(DATA[70] ^ 0xff) && buf[19] == DATA[69]);
    free(buf);
    buf = NULL;

    /* In-place repair restores the primary block; the next scan is clean. */
    ap_report_init(&rep);
    CK(ap_repair(P("chk.gpak"), &rep) == 0);
    CK(rep.repaired == 1 && rep.bad_primary == 1);
    ap_report_free(&rep);
    ap_report_init(&rep);
    CK(ap_check(P("chk.gpak"), &rep) == 0);
    CK(rep.issue_count == 0 && rep.bad_primary == 0);
    ap_report_free(&rep);
    CK(ap_extract(P("chk.gpak"), "tex/b.bin", &buf, &sz) == 0);
    CK(matches(buf, sz, "tex/b.bin"));
    free(buf);
}

static void test_unrecoverable(void) {
    make_fixture();
    CK(ap_write_checked(P("unrec.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    long copy_at = DATA_AT + 260;
    /* Damage block 2 in the primary and in the only copy. */
    poke(P("unrec.gpak"), DATA_AT + 2 * (long)BS + 5, (unsigned char)(DATA[2 * BS + 5] ^ 1));
    poke(P("unrec.gpak"), copy_at + 2 * (long)BS + 5, (unsigned char)(DATA[2 * BS + 5] ^ 1));

    ApReport rep;
    ap_report_init(&rep);
    CK(ap_check(P("unrec.gpak"), &rep) == 0);
    CK(rep.bad_primary == 1 && rep.issues[0].block == 2);
    CK(rep.issues[0].copies_ok == 0 && rep.issues[0].copies_bad == 1);
    CK(rep.unrecoverable == 1);
    /* Only z.bin spans block 2. */
    CK(rep.affected_entries == 1 && rep.missing_entries == 1);
    ap_report_free(&rep);

    unsigned char *buf = NULL;
    unsigned int sz = 0;
    ap_report_init(&rep);
    CK(ap_extract_ex(P("unrec.gpak"), "tex/z.bin", &buf, &sz, AP_LOAD_REPAIR, &rep) == AP_E_COPY);
    CK(rep.fault.block == 2 && rep.fault.entry == 2);
    CK(strcmp(rep.fault.name, "tex/z.bin") == 0);
    ap_report_free(&rep);
    ap_report_init(&rep);
    CK(ap_extract_ex(P("unrec.gpak"), "tex/a.bin", &buf, &sz, AP_LOAD_REPAIR, &rep) == 0);
    CK(matches(buf, sz, "tex/a.bin")); /* untouched entry still loads */
    free(buf);
    ap_report_free(&rep);

    ap_report_init(&rep);
    CK(ap_repair(P("unrec.gpak"), &rep) == AP_E_COPY);
    CK(rep.fault.block == 2 && rep.missing_entries == 1);
    ap_report_free(&rep);
}

static void test_errors(void) {
    make_fixture();

    /* Out-of-range block numbers are reported with the number asked for. */
    CK(ap_write_checked(P("err.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    ApReport rep;
    ap_report_init(&rep);
    CK(ap_repair_block(P("err.gpak"), 5, &rep) == AP_E_BLOCK);
    CK(rep.fault.block == 5);
    ap_report_free(&rep);
    ap_report_init(&rep);
    CK(ap_repair_block(P("err.gpak"), -1, &rep) == AP_E_BLOCK);
    CK(rep.fault.block == -1);
    ap_report_free(&rep);

    /* Missing copy: file cut inside the redundant region names the block. */
    CK(ap_write_checked(P("trunc_copy.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    truncate_at(P("trunc_copy.gpak"), DATA_AT + 260 + (long)BS + 10);
    ap_report_init(&rep);
    CK(ap_check(P("trunc_copy.gpak"), &rep) == AP_E_COPY);
    CK(rep.fault.block == 1);
    ap_report_free(&rep);

    /* Length: file cut inside the primary data names the block. */
    CK(ap_write_checked(P("trunc_data.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    truncate_at(P("trunc_data.gpak"), DATA_AT + 2 * (long)BS + 7);
    ap_report_init(&rep);
    CK(ap_check(P("trunc_data.gpak"), &rep) == AP_E_LENGTH);
    CK(rep.fault.block == 2);
    ap_report_free(&rep);

    /* Length: cut inside the layout block. */
    CK(ap_write_checked(P("trunc_lay.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    truncate_at(P("trunc_lay.gpak"), HDR_V2 + 6);
    ap_report_init(&rep);
    CK(ap_check(P("trunc_lay.gpak"), &rep) == AP_E_LENGTH);
    ap_report_free(&rep);

    /* Length: cut inside the checksum table. */
    CK(ap_write_checked(P("trunc_sum.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
    truncate_at(P("trunc_sum.gpak"), HDR_V2 + 12 + 6);
    ap_report_init(&rep);
    CK(ap_check(P("trunc_sum.gpak"), &rep) == AP_E_LENGTH);
    ap_report_free(&rep);

    /* A v1 pack has no block table: checked APIs refuse rather than guess. */
    CK(ap_write(P("legacy2.gpak"), ENT, 3, DATA, sizeof(DATA)) == 0);
    ap_report_init(&rep);
    CK(ap_check(P("legacy2.gpak"), &rep) == AP_E_VERSION);
    ap_report_free(&rep);
    ap_report_init(&rep);
    CK(ap_repair(P("legacy2.gpak"), &rep) == AP_E_VERSION);
    ap_report_free(&rep);

    /* A read-only pack still verifies, but repair says so instead of lying. */
    {
        CK(ap_write_checked(P("ro.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 1) == 0);
        poke(P("ro.gpak"), DATA_AT + 7, (unsigned char)(DATA[7] ^ 0xff));
        chmod(P("ro.gpak"), 0444);
        ap_report_init(&rep);
        CK(ap_check(P("ro.gpak"), &rep) == 0);
        CK(rep.bad_primary == 1 && rep.issues[0].block == 0);
        ap_report_free(&rep);
        ap_report_init(&rep);
        CK(ap_repair(P("ro.gpak"), &rep) == AP_E_READONLY);
        ap_report_free(&rep);
        chmod(P("ro.gpak"), 0644);
    }

    /* Bad arguments. */
    CK(ap_write_checked(P("arg.gpak"), ENT, 3, DATA, sizeof(DATA), 0, 1) == AP_E_ARG);
    CK(ap_write_checked(P("arg.gpak"), ENT, 3, DATA, sizeof(DATA), BS, 0) == AP_E_ARG);
    CK(ap_extract_ex(P("err.gpak"), "tex/a.bin", NULL, NULL, AP_LOAD_REPAIR, &rep) == AP_E_ARG);
}

/* Repair off must reproduce the historic loader bit for bit, including on a
 * damaged v1 pack (garbage in, same garbage out). */
static void test_repair_off_matches_legacy(void) {
    make_fixture();
    CK(ap_write(P("off.gpak"), ENT, 3, DATA, sizeof(DATA)) == 0);
    poke(P("off.gpak"), 8 + 3 * 40 + 4 + 10, 0xAA);

    unsigned char *a = NULL, *b = NULL;
    unsigned int as = 0, bs = 0;
    CK(ap_extract(P("off.gpak"), "tex/a.bin", &a, &as) == 0);
    CK(ap_extract_ex(P("off.gpak"), "tex/a.bin", &b, &bs, AP_LOAD_PLAIN, NULL) == 0);
    CK(as == bs && memcmp(a, b, as) == 0);
    CK(a[10] == 0xAA);
    free(a);
    free(b);
}

int main(void) {
    ensure_out();
    test_legacy();
    test_checked_roundtrip();
    test_unrecoverable();
    test_errors();
    test_repair_off_matches_legacy();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
