#ifndef MIXER_H
#define MIXER_H

/* Largest per-channel delay accepted by the aligner, in samples.
 * A delay must also leave room in the bus: after alignment the block is
 * frames + max_delay long, and bus.frames + frames + max_delay must be <=
 * bus.cap.  Anything outside that range fails with MX_ERR_DELAY. */
#define MX_MAX_DELAY 4096

/* Channels the compensation table can describe.  Buses may be wider for
 * plain mx_add(); mx_align()/mx_add_delayed() reject wider buses with
 * MX_ERR_CHANNELS unless table is NULL. */
#define MX_MAX_CHANNELS 32

/* Error codes.  mx_error_field() returns the name of the offending field. */
#define MX_ERR_ARG      (-1)   /* NULL pointer or frames <= 0 ("argument") */
#define MX_ERR_CHANNELS (-2)   /* channel count <= 0, or > MX_MAX_CHANNELS
                                  when a table is requested  ("channels")   */
#define MX_ERR_DELAY    (-3)   /* delay < 0, > MX_MAX_DELAY, or does not fit
                                  in the bus buffer            ("delay")    */
#define MX_ERR_CAPACITY (-4)   /* cap_frames <= 0               ("capacity") */
#define MX_ERR_IO       (-5)   /* file could not be written      ("path")     */
#define MX_ERR_MEM      (-6)   /* scratch allocation failed      ("memory")   */

typedef struct { float *buf; int frames; int channels; int cap; } MixBus;

/* Per-channel compensation table produced by mx_align()/mx_add_delayed().
 * Self-contained: pass &table, then read delay[c], comp[c], max_delay and
 * compensated.  mx_align() clears it first, so a partially filled table is
 * never handed back. */
typedef struct {
    int channels;      /* entries filled in delay[] and comp[]               */
    int max_delay;     /* largest arrival delay, in samples                  */
    int compensated;   /* total inserted samples: sum of comp[]              */
    int delay[MX_MAX_CHANNELS]; /* per-channel arrival delay from caller    */
    int comp[MX_MAX_CHANNELS];  /* per-channel padding = max_delay - delay[c] */
} MxAlignTable;

int mx_init(MixBus *b, int cap_frames, int channels);
void mx_free(MixBus *b);
int mx_add(MixBus *b, const float *src, int frames, float gain);
int mx_write_wav(const MixBus *b, const char *path);

/* Stage 1: per-channel sample alignment.
 * delays[c] is how many samples late channel c arrived.  Each channel is
 * padded by max_delay - delays[c], so an event recorded at frame t in
 * channel c lands at frame t + max_delay - delays[c] for every channel.
 * dst must hold (frames + max_delay) * channels floats and must not alias
 * src.  With all delays zero this is an exact copy and returns frames.
 * Returns the aligned frame count, or a negative MX_ERR_* code.  Arrays in
 * table are filled for table->channels entries; max_delay is the largest
 * delays[c], comp[c] = max_delay - delays[c], and compensated is their sum. */
int mx_align(const MixBus *b, const float *src, int frames, const int *delays,
             float *dst, MxAlignTable *table);

/* Stage 2: sum an aligned block (the output of mx_align) into the bus. */
int mx_add_aligned(MixBus *b, const float *aligned, int aligned_frames, float gain);

/* Both stages in one call.  On success returns the aligned frame count and,
 * when table is non-NULL, fills the compensation table. */
int mx_add_delayed(MixBus *b, const float *src, int frames, const int *delays,
                   float gain, MxAlignTable *table);

/* Name of the field rejected by err ("channels", "delay", ...).  */
const char *mx_error_field(int err);

#endif
