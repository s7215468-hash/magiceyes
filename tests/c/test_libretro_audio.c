/* Asset-free tests for the libretro audio clock, converter and FIFO policy.
 *
 * ME_TEST_SRC: host/libretro/audio.c
 */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "libretro/audio.h"

#define RING_BYTES (1u << 20)

typedef struct {
    me_lr_audio_t audio;
    uint8_t *ring;
    uint32_t read, write;
} fixture_t;

static int setup(void **state) {
    fixture_t *f = calloc(1, sizeof *f);
    if (!f) return -1;
    f->ring = calloc(1, RING_BYTES);
    if (!f->ring) { free(f); return -1; }
    *state = f;
    return 0;
}

static int teardown(void **state) {
    fixture_t *f = *state;
    free(f->ring);
    free(f);
    return 0;
}

static void put_byte(fixture_t *f, uint8_t v) {
    f->ring[f->write++ % RING_BYTES] = v;
}

static void put_s16_mono(fixture_t *f, int16_t v) {
    put_byte(f, (uint8_t)v);
    put_byte(f, (uint8_t)((uint16_t)v >> 8));
}

static void put_s16_stereo(fixture_t *f, int16_t l, int16_t r) {
    put_s16_mono(f, l);
    put_s16_mono(f, r);
}

static void fill_s16_mono(fixture_t *f, uint32_t frames, int16_t start) {
    for (uint32_t i = 0; i < frames; i++) put_s16_mono(f, (int16_t)(start + i));
}

static size_t prepare(fixture_t *f, uint32_t freq, uint32_t fmt, unsigned channels) {
    return me_lr_audio_prepare(&f->audio, f->ring, RING_BYTES, &f->read, f->write,
                               1, freq, fmt, channels);
}

static void test_clocked_cadence_and_cross_batch_upsampling(void **state) {
    fixture_t *f = *state;
    fill_s16_mono(f, 4096, 0);
    assert_int_equal(prepare(f, 22050, 0x8010, 1), 735);
    const int16_t *out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], 0);       /* source frame 0, first copy */
    assert_int_equal(out[2], 0);       /* source frame 0, second copy */
    assert_int_equal(out[4], 1);
    assert_int_equal(out[734 * 2], 367); /* first copy straddles the batch boundary */
    me_lr_audio_consume(&f->audio, 735);

    assert_int_equal(prepare(f, 22050, 0x8010, 1), 735);
    out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], 367);     /* second copy retained across retro_run() */
    assert_int_equal(out[2], 368);
    assert_int_equal(f->audio.pending, 735);
}

static void test_sixty_callbacks_are_exactly_one_audio_second(void **state) {
    fixture_t *f = *state;
    const uint32_t rates[] = {11025, 22050, 44100, 48000};
    for (size_t ri = 0; ri < sizeof rates / sizeof rates[0]; ri++) {
        uint32_t rate = rates[ri];
        me_lr_audio_reset(&f->audio); f->read = f->write = 0;
        fill_s16_mono(f, rate / 5, 100); /* 200ms: enough to start, below the 250ms cap */
        size_t total = 0;
        for (unsigned callback = 0; callback < 60; callback++) {
            fill_s16_mono(f, rate / 60 + 2, (int16_t)callback);
            size_t n = prepare(f, rate, 0x8010, 1);
            assert_int_equal(n, 735);
            total += n;
            me_lr_audio_consume(&f->audio, n);
        }
        assert_int_equal(total, 44100);
        uint32_t consumed = f->read / 2;
        assert_true(consumed + 1 >= rate && consumed <= rate + 1);
    }
}

static void test_pcm_formats_and_channel_mapping(void **state) {
    fixture_t *f = *state;
    const int16_t *out;

    for (unsigned i = 0; i < 1000; i++) put_byte(f, i ? 128 : 0);
    assert_int_equal(prepare(f, 44100, 0x0008, 1), 735); /* U8 */
    out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], -32768); assert_int_equal(out[1], -32768);

    me_lr_audio_reset(&f->audio); f->read = f->write = 0;
    for (unsigned i = 0; i < 1000; i++) put_byte(f, i ? 0 : 0x80);
    assert_int_equal(prepare(f, 44100, 0x8008, 1), 735); /* S8 */
    out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], -32768); assert_int_equal(out[2], 0);

    me_lr_audio_reset(&f->audio); f->read = f->write = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint16_t v = i ? 32768 : 0;
        put_byte(f, (uint8_t)v); put_byte(f, (uint8_t)(v >> 8));
    }
    assert_int_equal(prepare(f, 44100, 0x0010, 1), 735); /* U16 */
    out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], -32768); assert_int_equal(out[2], 0);

    me_lr_audio_reset(&f->audio); f->read = f->write = 0;
    for (unsigned i = 0; i < 1000; i++) put_s16_stereo(f, 1234, -2345);
    assert_int_equal(prepare(f, 44100, 0x8010, 2), 735); /* S16 stereo */
    out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[0], 1234); assert_int_equal(out[1], -2345);
}

static void test_short_sparse_sound_eventually_starts(void **state) {
    fixture_t *f = *state;
    for (unsigned i = 0; i < 100; i++) put_s16_mono(f, 1234); /* 2.3ms, below 20ms */
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 0);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 0);
    assert_int_equal(f->read, 0);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    const int16_t *out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[99 * 2], 1234);
    assert_int_equal(out[100 * 2], 0); /* true underrun is padded, not stalled forever */
}

static void test_running_stream_silence_fills_an_underrun(void **state) {
    fixture_t *f = *state;
    for (unsigned i = 0; i < 882; i++) put_s16_mono(f, 2222); /* exactly 20ms */
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    me_lr_audio_consume(&f->audio, 735);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    const int16_t *out = me_lr_audio_output(&f->audio);
    assert_int_equal(out[146 * 2], 2222);
    assert_int_equal(out[147 * 2], 0);
    assert_int_equal(out[734 * 2], 0);
}

static void test_partial_consumption_blocks_new_source_reads(void **state) {
    fixture_t *f = *state;
    fill_s16_mono(f, 4096, 0);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    uint32_t read_after_batch = f->read;
    me_lr_audio_consume(&f->audio, 17);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 718);
    assert_int_equal(f->read, read_after_batch);
    me_lr_audio_consume(&f->audio, 718);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    assert_true(f->read > read_after_batch);
}

static void test_format_reopen_discards_old_pending_audio(void **state) {
    fixture_t *f = *state;
    fill_s16_mono(f, 1000, 1234);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    assert_int_equal(f->audio.pending, 735);

    f->read = f->write = 0; /* SDL_OpenAudio resets both cursors */
    for (unsigned i = 0; i < 1000; i++) put_byte(f, 128);
    assert_int_equal(prepare(f, 44100, 0x0008, 1), 735);
    assert_int_equal(me_lr_audio_output(&f->audio)[0], 0);
}

static void test_reset_flushes_audio_for_state_load(void **state) {
    fixture_t *f = *state;
    fill_s16_mono(f, 1000, 1234);
    assert_int_equal(prepare(f, 44100, 0x8010, 1), 735);
    assert_true(f->audio.pending && f->audio.started && f->audio.have_hold);
    me_lr_audio_reset(&f->audio);
    assert_int_equal(f->audio.pending, 0);
    assert_int_equal(f->audio.started, 0);
    assert_int_equal(f->audio.have_hold, 0);
    assert_int_equal(f->audio.source_phase, 0);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_clocked_cadence_and_cross_batch_upsampling, setup, teardown),
        cmocka_unit_test_setup_teardown(test_sixty_callbacks_are_exactly_one_audio_second, setup, teardown),
        cmocka_unit_test_setup_teardown(test_pcm_formats_and_channel_mapping, setup, teardown),
        cmocka_unit_test_setup_teardown(test_short_sparse_sound_eventually_starts, setup, teardown),
        cmocka_unit_test_setup_teardown(test_running_stream_silence_fills_an_underrun, setup, teardown),
        cmocka_unit_test_setup_teardown(test_partial_consumption_blocks_new_source_reads, setup, teardown),
        cmocka_unit_test_setup_teardown(test_format_reopen_discards_old_pending_audio, setup, teardown),
        cmocka_unit_test_setup_teardown(test_reset_flushes_audio_for_state_load, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
