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

/* 去重打包错误码（ap_pack）；err_entry 指明出错条目下标 */
#define AP_ERR_EMPTY (-6)  /* 条目数为零 */
#define AP_ERR_ALIAS (-7)  /* 别名为空（条目名为空串） */
#define AP_ERR_HASH  (-8)  /* 哈希冲突：哈希相同但内容不同 */

typedef unsigned long long (*ApHashFn)(const unsigned char *p, unsigned int n);
unsigned long long ap_hash(const unsigned char *p, unsigned int n); /* FNV-1a 64 */

/* 去重打包，两段式：先按内容哈希合并重复项，再重映射索引并保留别名。
 * dedup=0 时与 ap_write 完全一致（读取结果不变）；
 * dedup=1 时重复内容只存一份，所有条目名（别名）保留在索引中。
 * hf 为 NULL 时用 ap_hash；可注入弱哈希以测试冲突路径。
 * 出错时若 err_entry 非 NULL，写入导致错误的条目下标。 */
int ap_pack(const char *path, const ApEntry *entries, int n, const unsigned char *data,
            unsigned int data_size, int dedup, ApHashFn hf, int *err_entry);
#endif
