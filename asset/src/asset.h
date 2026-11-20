#ifndef ASSET_H
#define ASSET_H
typedef struct {
    char name[32];
    unsigned int size;
    unsigned int offset;
} ApEntry;

typedef struct {
    ApEntry *entries;
    int count;
    unsigned int data_size;
} ApIndex;

int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size);
int ap_read(const char *path, ApIndex *idx);
int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size);
void ap_free(ApIndex *idx);
#endif
