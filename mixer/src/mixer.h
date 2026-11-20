#ifndef MIXER_H
#define MIXER_H
typedef struct { float *buf; int frames; int channels; int cap; } MixBus;
int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);
#endif
