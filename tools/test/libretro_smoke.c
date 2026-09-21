/* Tiny desktop libretro frontend used to smoke-test the magiceyes shared core without requiring
 * RetroArch. It deliberately exercises load -> run -> unload twice when two games are supplied. */
#include "libretro.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_system_dir, *g_save_dir;
static unsigned g_frames, g_nonzero;
static unsigned long long g_audio_frames;
static unsigned g_audio_calls;
static size_t g_max_audio_batch;
static unsigned g_geometry_updates;
static bool g_fastforward;

static bool RETRO_CALLCONV environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir; return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g_geometry_updates++; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "magiceyes_timing"))
            v->value = getenv("ME_LIBRETRO_ASYNC") ? "Asynchronous" : "Synchronized";
        else if (!strcmp(v->key, "magiceyes_pointer")) v->value = "Enabled";
        else v->value = NULL;
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = g_fastforward; return true;
    default:
        return false;
    }
}
static void RETRO_CALLCONV video(const void *data, unsigned w, unsigned h, size_t pitch) {
    g_frames++;
    if (!data) return;
    const unsigned char *p = data;
    for (unsigned y = 0; y < h; y += 16)
        for (unsigned x = 0; x < w * 2; x += 31)
            if (p[(size_t)y * pitch + x]) { g_nonzero++; return; }
}
static void RETRO_CALLCONV audio(int16_t l, int16_t r) { (void)l; (void)r; g_audio_frames++; }
static size_t RETRO_CALLCONV audio_batch(const int16_t *p, size_t n) {
    (void)p;
    g_audio_calls++;
    if (n > g_max_audio_batch) g_max_audio_batch = n;
    size_t used = getenv("ME_LIBRETRO_PARTIAL_AUDIO") && n > 17 ? 17 : n;
    g_audio_frames += used; return used;
}
static void RETRO_CALLCONV input_poll(void) {}
static int16_t RETRO_CALLCONV input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p; (void)d; (void)i; (void)id; return 0;
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

#define LOAD(name) do { *(void **)(&name) = dlsym(lib, #name); \
    if (!(name)) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s CORE GAME SYSTEM_DIR SAVE_DIR [SECOND_GAME]\n", argv[0]);
        return 2;
    }
    g_system_dir = argv[3]; g_save_dir = argv[4];
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    unsigned (*retro_api_version)(void); void (*retro_init)(void), (*retro_deinit)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_unload_game)(void), (*retro_run)(void), (*retro_reset)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void *, size_t), (*retro_unserialize)(const void *, size_t);
    LOAD(retro_api_version); LOAD(retro_init); LOAD(retro_deinit); LOAD(retro_set_environment);
    LOAD(retro_set_video_refresh); LOAD(retro_set_audio_sample); LOAD(retro_set_audio_sample_batch);
    LOAD(retro_set_input_poll); LOAD(retro_set_input_state); LOAD(retro_load_game);
    LOAD(retro_unload_game); LOAD(retro_run); LOAD(retro_reset);
    LOAD(retro_serialize_size); LOAD(retro_serialize); LOAD(retro_unserialize);
    if (retro_api_version() != RETRO_API_VERSION) return 2;
    retro_set_environment(environment); retro_set_video_refresh(video);
    retro_set_audio_sample(audio); retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll); retro_set_input_state(input_state); retro_init();
    if (getenv("ME_LIBRETRO_FAIL_FIRST")) {
        struct retro_game_info bad = { "/definitely/not/a/magiceyes-game.gpe", NULL, 0, NULL };
        if (retro_load_game(&bad)) { fprintf(stderr, "invalid content unexpectedly loaded\n"); return 1; }
    }
    int games = argc > 5 ? 2 : 1;
    for (int g = 0; g < games; g++) {
        struct retro_game_info gi = { g ? argv[5] : argv[2], NULL, 0, NULL };
        g_frames = g_nonzero = 0; g_audio_frames = 0; g_geometry_updates = 0;
        g_audio_calls = 0; g_max_audio_batch = 0;
        if (!retro_load_game(&gi)) { fprintf(stderr, "load failed: %s\n", gi.path); return 1; }
        int runs = getenv("ME_LIBRETRO_RESET_TEST") ? 600 : 360;
        if (getenv("ME_LIBRETRO_RUNS")) runs = atoi(getenv("ME_LIBRETRO_RUNS"));
        void *state = NULL; size_t state_size = 0;
        for (int i = 0; i < runs; i++) {
            g_fastforward = getenv("ME_LIBRETRO_FASTFORWARD") != NULL ||
                (getenv("ME_LIBRETRO_FASTFORWARD_TOGGLE") && i >= 120 && i < 240);
            retro_run();
            if (i == 180 && getenv("ME_LIBRETRO_RESET_TEST")) retro_reset();
            if (i == 180 && (getenv("ME_LIBRETRO_STATE_TEST") ||
                             getenv("ME_LIBRETRO_STATE_V1_TEST"))) {
                state_size = retro_serialize_size();
                state = malloc(state_size);
                if (!state || !retro_serialize(state, state_size)) {
                    fprintf(stderr, "serialize failed (%zu bytes)\n", state_size); return 1;
                }
                if (getenv("ME_LIBRETRO_STATE_V1_TEST")) {
                    unsigned char *p = state;
                    uint32_t body = get_u32(p + 12);
                    if ((size_t)body + 40 > state_size) {
                        fprintf(stderr, "state too large for v1 compatibility fixture\n"); return 1;
                    }
                    memmove(p + 40, p + 32, body);
                    put_u32(p + 8, 1);  /* old adapter-state version */
                    put_u32(p + 24, 2); /* two stale stereo frames, discarded on load */
                    memset(p + 32, 0x5a, 8);
                }
            }
            if (i == 240 && state) {
                if (!retro_unserialize(state, state_size)) {
                    fprintf(stderr, "unserialize failed\n"); return 1;
                }
                free(state); state = NULL;
            }
            if (i == 180 && getenv("ME_LIBRETRO_PAUSE_TEST")) usleep(500000);
            else if (getenv("ME_LIBRETRO_ASYNC")) usleep(16667);
        }
        g_fastforward = false;
        free(state);
        retro_unload_game();
        printf("game=%s callbacks=%u nonzero=%u audio_frames=%llu audio_calls=%u"
               " max_audio_batch=%zu geometry=%u\n",
               gi.path, g_frames, g_nonzero, g_audio_frames, g_audio_calls,
               g_max_audio_batch, g_geometry_updates);
        if (!g_frames || !g_geometry_updates) return 1;
        if (g_max_audio_batch > 735 || g_audio_frames > (unsigned long long)runs * 735) {
            fprintf(stderr, "audio pacing exceeded the 44.1kHz/60Hz callback budget\n");
            return 1;
        }
    }
    retro_deinit();
    dlclose(lib);
    return 0;
}
