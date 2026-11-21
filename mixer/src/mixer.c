#define _POSIX_C_SOURCE 199309L
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int mx_init(MixBus *b, int cap_frames, int channels) {
    if (!b || cap_frames <= 0 || channels <= 0) return -1;
    b->buf = (float*)calloc((size_t)cap_frames * (size_t)channels, sizeof(float));
    if (!b->buf) return -1;
    b->frames = 0; b->channels = channels; b->cap = cap_frames;
    return 0;
}

void mx_free(MixBus *b) { if (b) { free(b->buf); b->buf = NULL; b->frames = 0; b->cap = 0; } }

int mx_add(MixBus *b, const float *src, int frames, float gain) {
    if (!b || !src || frames <= 0) return -1;
    int n = frames;
    if (b->frames + n > b->cap) n = b->cap - b->frames;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < b->channels; ++c)
            b->buf[(size_t)(b->frames + i) * b->channels + c] += src[(size_t)i * b->channels + c] * gain;
    b->frames += n;
    return n;
}

int mx_mix_into(MixBus *b, const float *src, int frames, float gain) {
    if (!b || !src || frames <= 0) return -1;
    int n = frames;
    if (n > b->cap) n = b->cap;
    /* 从帧 0 起同位置累加；b->frames 记录已占用的最大帧范围，写游标不后移 */
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < b->channels; ++c)
            b->buf[(size_t)i * b->channels + c] += src[(size_t)i * b->channels + c] * gain;
    if (n > b->frames) b->frames = n;
    return n;
}

int mx_write_wav(const MixBus *b, const char *path) {
    if (!b || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    unsigned int data_bytes = (unsigned int)(b->frames * b->channels * 2);
    unsigned int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    unsigned int sz = 16; fwrite(&sz, 4, 1, f);
    unsigned short fmt = 1, ch = (unsigned short)b->channels;
    unsigned int rate = 48000, byte_rate = rate * ch * 2;
    unsigned short align = (unsigned short)(ch * 2), bits = 16;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    for (int i = 0; i < b->frames; ++i)
        for (int c = 0; c < b->channels; ++c) {
            float v = b->buf[(size_t)i * b->channels + c];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            short s = (short)lrintf(v * 32767.0f);
            fwrite(&s, 2, 1, f);
        }
    fclose(f);
    return 0;
}

/* ---------- solo / mute 三段式混音 ---------- */

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static int has_id(const int *set, int n, int id) {
    for (int i = 0; i < n; ++i) if (set[i] == id) return 1;
    return 0;
}

/* 把 id 追加进动态数组，已存在则忽略；失败返回 -1 */
static int append_id(int **arr, int *count, int id) {
    if (has_id(*arr, *count, id)) return 0;
    int *p = (int*)realloc(*arr, sizeof(int) * (size_t)(*count + 1));
    if (!p) return -1;
    *arr = p;
    (*arr)[(*count)++] = id;
    return 0;
}

static void reject_init(MixReject *r) {
    r->out_of_range = NULL; r->out_of_range_count = 0;
    r->duplicate = NULL;    r->duplicate_count = 0;
}

void mx_reject_free(MixReject *r) {
    if (!r) return;
    free(r->out_of_range); r->out_of_range = NULL;
    free(r->duplicate);     r->duplicate = NULL;
}

int mx_validate(int nsrc,
                const int *solo, int nsolo,
                const int *mute, int nmute,
                MixReject *err) {
    if (nsrc <= 0 || nsolo < 0 || nmute < 0 ||
        (nsolo > 0 && !solo) || (nmute > 0 && !mute))
        return MX_ERR_ARG;

    MixReject tmp;
    MixReject *e = err ? err : &tmp;
    reject_init(e);

    /* 越界通道号 + 集合内重复，solo 与 mute 各自检查。
       注意：同一通道同时出现在两个集合是合法的三段式组合
       （段1 通过、段2 屏蔽），不算冲突，故不在此拒绝。 */
    for (int i = 0; i < nsolo; ++i) {
        if (solo[i] < 0 || solo[i] >= nsrc)
            append_id(&e->out_of_range, &e->out_of_range_count, solo[i]);
        else
            for (int j = 0; j < i; ++j)
                if (solo[j] == solo[i])
                    append_id(&e->duplicate, &e->duplicate_count, solo[i]);
    }
    for (int i = 0; i < nmute; ++i) {
        if (mute[i] < 0 || mute[i] >= nsrc)
            append_id(&e->out_of_range, &e->out_of_range_count, mute[i]);
        else
            for (int j = 0; j < i; ++j)
                if (mute[j] == mute[i])
                    append_id(&e->duplicate, &e->duplicate_count, mute[i]);
    }

    int rc = MX_OK;
    if (e->out_of_range_count) rc = MX_ERR_RANGE;
    else if (e->duplicate_count) rc = MX_ERR_DUP;

    if (!err) mx_reject_free(&tmp);
    return rc;
}

int mx_mix(MixBus *buses, int nbus,
           const MixSource *srcs, int nsrc,
           const int *solo, int nsolo,
           const int *mute, int nmute,
           MixStats *stats, MixReject *err) {
    if (!buses || nbus <= 0 || !srcs || nsrc <= 0) return MX_ERR_ARG;

    MixReject tmp;
    MixReject *e = err ? err : &tmp;
    reject_init(e);

    int rc = mx_validate(nsrc, solo, nsolo, mute, nmute, e);
    if (rc != MX_OK) {
        if (!err) mx_reject_free(&tmp);
        return rc;
    }

    int *kept = (int*)malloc(sizeof(int) * (size_t)nsrc);
    if (!kept) { if (!err) mx_reject_free(&tmp); return MX_ERR_ARG; }

    long t0 = now_ns();

    /* 段1：按 solo 集合筛选。集合为空 = 未开启 solo，全部通过（与旧路径一致） */
    int npass = 0;
    for (int i = 0; i < nsrc; ++i) {
        if (nsolo == 0 || has_id(solo, nsolo, i)) kept[npass++] = i;
    }
    long t1 = now_ns();

    /* 段2：按 mute 屏蔽，保留未被静音的源 */
    int nlive = 0, nmuted = 0;
    for (int k = 0; k < npass; ++k) {
        int i = kept[k];
        if (has_id(mute, nmute, i)) { ++nmuted; continue; }
        kept[nlive++] = i;
    }
    long t2 = now_ns();

    /* 段3：按总线汇总；总线号非法在此拦截，且总线尚未被写入 */
    for (int k = 0; k < nlive; ++k) {
        if (srcs[kept[k]].bus < 0 || srcs[kept[k]].bus >= nbus) {
            free(kept);
            if (!err) mx_reject_free(&tmp);
            return MX_ERR_BUS;
        }
    }
    int nagg = 0;
    for (int k = 0; k < nlive; ++k) {
        const MixSource *s = &srcs[kept[k]];
        int added = mx_mix_into(&buses[s->bus], s->data, s->frames, s->gain);
        if (added < 0) {
            free(kept);
            if (!err) mx_reject_free(&tmp);
            return MX_ERR_ARG;
        }
        ++nagg;
    }
    long t3 = now_ns();

    if (stats) {
        stats->stages = 3;
        stats->solo_passed = npass;
        stats->muted = nmuted;
        stats->aggregated = nagg;
        stats->nsec_stage[0] = t1 - t0;
        stats->nsec_stage[1] = t2 - t1;
        stats->nsec_stage[2] = t3 - t2;
        stats->nsec_total = t3 - t0;
    }
    free(kept);
    if (!err) mx_reject_free(&tmp);
    return MX_OK;
}
