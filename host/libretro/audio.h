#ifndef MAGICEYES_LIBRETRO_AUDIO_H
#define MAGICEYES_LIBRETRO_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define ME_LR_AUDIO_OUTPUT_RATE 44100u
#define ME_LR_AUDIO_OUTPUT_FPS 60u
#define ME_LR_AUDIO_MAX_OUTPUT 16384u

typedef struct {
    uint64_t source_phase;
    uint32_t budget_phase;
    int16_t output[ME_LR_AUDIO_MAX_OUTPUT * 2];
    size_t pending;
    int16_t hold_l, hold_r;
    int have_hold, started;
    unsigned start_wait;
    uint32_t seen_freq, seen_fmt, seen_channels;
} me_lr_audio_t;

/* Reset is also the save-state load policy: frontend audio and source-ring PCM are flushed, then
   the restored guest establishes a fresh cushion. Replaying pre-load PCM after a load is worse
   than a short rebuffer because it produces a stale burst from the abandoned timeline. */
void me_lr_audio_reset(me_lr_audio_t *audio);

/* Prepare at most one 44.1 kHz / 60 Hz batch. Existing partially-consumed output is returned
   without reading more source PCM. read_cursor and write_cursor are monotonic byte cursors. */
size_t me_lr_audio_prepare(me_lr_audio_t *audio,
                           const uint8_t *ring, uint32_t ring_size,
                           volatile uint32_t *read_cursor, uint32_t write_cursor,
                           int active, uint32_t freq, uint32_t fmt, unsigned channels);

const int16_t *me_lr_audio_output(const me_lr_audio_t *audio);
void me_lr_audio_consume(me_lr_audio_t *audio, size_t frames);

#endif
