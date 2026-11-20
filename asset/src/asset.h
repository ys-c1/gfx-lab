#ifndef ASSET_H
#define ASSET_H

/* On-disk versions.  Version 1 is the historical pack layout and stays
 * byte-for-byte compatible.  Version 2 adds a per-block checksum table and
 * whole-data redundant copies. */
#define AP_VERSION_LEGACY  1
#define AP_VERSION_CHECKED 2

/* Flags for ap_extract_ex().  AP_LOAD_PLAIN (0) is exactly ap_extract(). */
#define AP_LOAD_PLAIN  0u
#define AP_LOAD_VERIFY 1u   /* reject a corrupt block, do not repair */
#define AP_LOAD_REPAIR 2u   /* replace a corrupt block from a good copy */

/* Error codes.  The first five are also returned by the legacy API. */
enum {
    AP_E_ARG      = -1,
    AP_E_IO       = -2,
    AP_E_FORMAT   = -3,
    AP_E_VERSION  = -4,
    AP_E_NOTFOUND = -5,
    AP_E_LENGTH   = -6,   /* file shorter than declared; fault.block is set */
    AP_E_COPY     = -7,   /* no usable redundant copy; fault.block is set */
    AP_E_BLOCK    = -8,   /* block index outside [0, block_count) */
    AP_E_CORRUPT  = -9,   /* block checksum failed and repair was not requested */
    AP_E_READONLY = -10   /* repair is impossible: the pack is not writable */
};

typedef struct {
    char name[32];
    unsigned int size;
    unsigned int offset;
} ApEntry;

typedef struct {
    ApEntry *entries;
    int count;
    unsigned int data_size;
    int version;            /* AP_VERSION_LEGACY or AP_VERSION_CHECKED */
    unsigned int block_count; /* 0 for legacy packs */
} ApIndex;

/* Legacy API: unchanged behaviour, v1 packs still read exactly as before.
 * v2 packs are accepted too; the checksum/replica trailer is ignored. */
int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size);
int ap_read(const char *path, ApIndex *idx);
int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size);
void ap_free(ApIndex *idx);

/* Stage 1: write a v2 pack.  Each block of `block_size` bytes gets a FNV-1a
 * checksum; `copies` complete copies of the data region are appended. */
int ap_write_checked(const char *path, const ApEntry *entries, int n,
                     const unsigned char *data, unsigned int data_size,
                     unsigned int block_size, int copies);

/* Fault record filled by verify/repair failures.  block is -1 when the fault
 * is not tied to a data block (header, index table). */
typedef struct {
    int code;             /* AP_E_* or 0 */
    int block;            /* offending block, -1 if not block-specific */
    int entry;            /* first affected entry, -1 if none */
    char name[32];        /* that entry's name ("" if none) */
    unsigned int offset;  /* byte offset in the pack, where meaningful */
} ApFault;

typedef struct {
    int block;
    int entries;          /* entries overlapping the block */
    int first_entry;
    char first_name[32];
    int primary_ok;
    int copies_ok;        /* copies present and matching the checksum */
    int copies_bad;       /* copies missing, truncated or mismatching */
    int repaired;         /* block was restored from a copy */
} ApBlockIssue;

typedef struct {
    int block_size;
    int block_count;
    int copies;
    int entry_count;
    int bad_primary;      /* primary blocks whose checksum failed */
    int bad_copies;       /* redundant blocks whose checksum failed */
    int repaired;         /* blocks actually rewritten from a copy */
    int unrecoverable;    /* bad primary blocks with no usable copy */
    int affected_entries; /* entries overlapping any bad primary block */
    int missing_entries;  /* entries with at least one unrecoverable block */
    int issue_count;
    ApBlockIssue *issues; /* malloc'd, issue_count elements */
    ApFault fault;
} ApReport;

/* Must be called before the first use of a report; ap_report_free() releases
 * the issue array. */
void ap_report_init(ApReport *rep);
void ap_report_free(ApReport *rep);

/* Stage 2: verify every block and locate damage down to entries.
 * Returns 0 when the scan completed (even if bad blocks were found; look at
 * the report).  A file too short to hold the primary data returns AP_E_LENGTH;
 * one too short to hold a declared redundant copy returns AP_E_COPY.  In both
 * cases rep->fault.block / fault.entry / fault.name identify the block and the
 * first texture it damages. */
int ap_check(const char *path, ApReport *rep);

/* Load one entry with verification and/or in-memory repair.  With flags ==
 * AP_LOAD_PLAIN the result is identical to ap_extract(). */
int ap_extract_ex(const char *path, const char *name,
                  unsigned char **out, unsigned int *out_size,
                  unsigned int flags, ApReport *rep);

/* Stage 3: restore every repairable primary block in place.  Returns
 * AP_E_COPY (fault identifies the block and entry) if some block has no
 * usable copy; repaired blocks are still written before the error returns.
 * Returns AP_E_READONLY if the pack cannot be opened for writing. */
int ap_repair(const char *path, ApReport *rep);

/* Repair a single block from a redundant copy.  Out-of-range block numbers
 * return AP_E_BLOCK with fault.block set to the requested number. */
int ap_repair_block(const char *path, int block, ApReport *rep);

#endif
