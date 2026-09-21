#include "audio.h"

#include <string.h>

#define AUDIO_PREBUFFER_MS 20u
#define AUDIO_START_WAIT_CALLBACKS 3u

static int16_t pcm_sample(const uint8_t *p, unsigned bits, int is_signed) {
    if (bits == 8) return is_signed ? (int16_t)((int8_t)p[0] * 256)
                                    : (int16_t)(((int)p[0] - 128) << 8);
    uint16_t u = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return is_signed ? (int16_t)u : (int16_t)((int)u - 32768);
}

static void reset_source(me_lr_audio_t *audio) {
    audio->source_phase = 0;
    audio->pending = 0;
    audio->hold_l = audio->hold_r = 0;
    audio->have_hold = audio->started = 0;
    audio->start_wait = 0;
    audio->seen_freq = audio->seen_fmt = audio->seen_channels = 0;
}

void me_lr_audio_reset(me_lr_audio_t *audio) {
    if (!audio) return;
    reset_source(audio);
    audio->budget_phase = 0;
}

const int16_t *me_lr_audio_output(const me_lr_audio_t *audio) {
    return audio ? audio->output : NULL;
}

void me_lr_audio_consume(me_lr_audio_t *audio, size_t frames) {
    if (!audio || !audio->pending) return;
    if (frames > audio->pending) frames = audio->pending;
    audio->pending -= frames;
    if (audio->pending)
        memmove(audio->output, audio->output + frames * 2,
                audio->pending * 2 * sizeof *audio->output);
}

size_t me_lr_audio_prepare(me_lr_audio_t *audio,
                           const uint8_t *ring, uint32_t ring_size,
                           volatile uint32_t *read_cursor, uint32_t write_cursor,
                           int active, uint32_t freq, uint32_t fmt, unsigned channels) {
    if (!audio || !ring || !ring_size || !read_cursor) return 0;
    unsigned bits = fmt & 0xffu;
    if (!active || !freq || (channels != 1 && channels != 2) ||
        (bits != 8 && bits != 16)) {
        me_lr_audio_reset(audio);
        return 0;
    }
    if (freq != audio->seen_freq || fmt != audio->seen_fmt ||
        channels != audio->seen_channels) {
        uint32_t budget_phase = audio->budget_phase;
        reset_source(audio);
        audio->budget_phase = budget_phase;
        audio->seen_freq = freq;
        audio->seen_fmt = fmt;
        audio->seen_channels = channels;
    }
    if (audio->pending) return audio->pending;

    unsigned bytes_per = bits / 8;
    unsigned frame_bytes = channels * bytes_per;
    uint32_t avail = write_cursor - *read_cursor;
    avail -= avail % frame_bytes;

    /* Retain at most 250 ms after a frontend stall. The producer normally maintains a much
       smaller cushion; this is only the recovery path for a menu pause or blocked frontend. */
    uint32_t keep = freq * frame_bytes / 4;
    if (avail > keep) {
        uint32_t drop = avail - keep;
        drop -= drop % frame_bytes;
        *read_cursor += drop;
        avail -= drop;
        audio->source_phase = 0;
        audio->have_hold = 0;
    }

    if (!audio->started) {
        uint32_t prebuffer = freq * frame_bytes * AUDIO_PREBUFFER_MS / 1000u;
        if (prebuffer < frame_bytes) prebuffer = frame_bytes;
        prebuffer -= prebuffer % frame_bytes;
        if (avail < prebuffer) {
            /* A direct /dev/dsp writer may emit one short effect and then stop. Once any PCM has
               waited for three frontend callbacks, play it instead of waiting forever for 20ms. */
            if (!avail) { audio->start_wait = 0; return 0; }
            if (++audio->start_wait < AUDIO_START_WAIT_CALLBACKS) return 0;
        }
        audio->started = 1;
        audio->start_wait = 0;
    }

    audio->budget_phase += ME_LR_AUDIO_OUTPUT_RATE;
    size_t target = audio->budget_phase / ME_LR_AUDIO_OUTPUT_FPS;
    audio->budget_phase %= ME_LR_AUDIO_OUTPUT_FPS;
    if (target > ME_LR_AUDIO_MAX_OUTPUT) target = ME_LR_AUDIO_MAX_OUTPUT;

    uint32_t r = *read_cursor;
    uint32_t source_frames = avail / frame_bytes;
    size_t out_n = 0;
    uint32_t used_source = 0;
    int is_signed = (fmt & 0x8000u) != 0;
    while (out_n < target) {
        /* source_phase is source time in output-rate units. Retaining the held sample across
           calls keeps upsampling continuous when an odd output batch splits a duplicated sample. */
        while (!audio->have_hold || audio->source_phase >= ME_LR_AUDIO_OUTPUT_RATE) {
            if (used_source >= source_frames) goto underrun;
            uint8_t raw[4] = {0};
            for (unsigned k = 0; k < frame_bytes; k++)
                raw[k] = ring[(r + used_source * frame_bytes + k) % ring_size];
            audio->hold_l = pcm_sample(raw, bits, is_signed);
            audio->hold_r = channels == 2 ? pcm_sample(raw + bytes_per, bits, is_signed)
                                          : audio->hold_l;
            used_source++;
            if (audio->have_hold) audio->source_phase -= ME_LR_AUDIO_OUTPUT_RATE;
            else audio->have_hold = 1;
        }
        audio->output[out_n * 2] = audio->hold_l;
        audio->output[out_n * 2 + 1] = audio->hold_r;
        out_n++;
        audio->source_phase += freq;
    }
underrun:
    *read_cursor = r + used_source * frame_bytes;
    if (out_n < target)
        memset(audio->output + out_n * 2, 0,
               (target - out_n) * 2 * sizeof *audio->output);
    audio->pending = target;
    return audio->pending;
}
