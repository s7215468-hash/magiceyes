/* magiceyes libretro adapter.
 *
 * The emulator remains asynchronous internally: its main ARM CPU, guest workers and helper
 * presenter keep their existing threading model. retro_run publishes the newest completed
 * RGB565 frame, feeds input into the shared host bridge and drains its PCM ring. This preserves
 * the standalone engine's broad title compatibility while giving the frontend ownership of all
 * host video, audio and input APIs. */
#include "libretro.h"
#include "engine.h"
#include "paths.h"

#ifndef ME_VERSION
#define ME_VERSION "0.2.0-dev"
#endif

#define OUTPUT_RATE 44100u
#define MAX_AUDIO_OUT 8192u

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static pthread_t engine_thread;
static pthread_t monitor_thread;
static volatile int engine_thread_done;
static int engine_thread_valid;
static volatile int monitor_stop, monitor_paused;
static volatile uint64_t run_heartbeat;
static char content_path[PATH_MAX];
static char engine_argv0[PATH_MAX];
static uint64_t audio_phase;
static int16_t audio_out[MAX_AUDIO_OUT * 2];

static void RETRO_CALLCONV null_video(const void *p, unsigned w, unsigned h, size_t pitch) {
    (void)p; (void)w; (void)h; (void)pitch;
}
static void RETRO_CALLCONV null_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t RETRO_CALLCONV null_audio_batch(const int16_t *p, size_t n) { (void)p; return n; }
static void RETRO_CALLCONV null_input_poll(void) {}
static int16_t RETRO_CALLCONV null_input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
    (void)p; (void)d; (void)i; (void)id; return 0;
}

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (log_cb) {
        char line[1024];
        vsnprintf(line, sizeof line, fmt, ap);
        log_cb(level, "%s", line);
    } else {
        vfprintf(level >= RETRO_LOG_ERROR ? stderr : stdout, fmt, ap);
    }
    va_end(ap);
}

static void join_path(char *out, size_t cap, const char *base, const char *tail) {
    if (!base || !*base) base = ".";
    size_t n = strlen(base);
    snprintf(out, cap, "%s%s%s", base, n && (base[n-1] == '/' || base[n-1] == '\\') ? "" : "/", tail);
}

static void *engine_entry(void *unused) {
    (void)unused;
    char *argv[3] = { engine_argv0, content_path, NULL };
    me_engine_main(2, argv);
    /* Early argument/content failures return before main's normal cleanup path. These are
       idempotent and ensure a failed load never leaves process-global handlers installed. */
    guard_shutdown();
    me_platform_shutdown();
    engine_thread_done = 1;
    return NULL;
}

/* libretro has no explicit "frontend paused" callback. Detect a frontend that stopped calling
   retro_run and park every guest CPU through the existing debugger stop-the-world primitive.
   This prevents games and audio from running behind a paused RetroArch menu. */
static void *monitor_entry(void *unused) {
    (void)unused;
    uint64_t seen = 0;
    double changed = host_now();
    while (!monitor_stop) {
        uint64_t now_seen = run_heartbeat;
        double now = host_now();
        if (now_seen != seen) { seen = now_seen; changed = now; }
        if (seen && !monitor_paused && now - changed > 0.12) {
            if (dbg_pause(500, NULL, NULL, NULL) == 0) monitor_paused = 1;
        }
        me_usleep(10000);
    }
    if (monitor_paused) { dbg_resume(); monitor_paused = 0; }
    return NULL;
}

static int pressed(unsigned id) {
    return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

static void update_input(void) {
    uint32_t b = 0;
    int up = pressed(RETRO_DEVICE_ID_JOYPAD_UP);
    int dn = pressed(RETRO_DEVICE_ID_JOYPAD_DOWN);
    int lf = pressed(RETRO_DEVICE_ID_JOYPAD_LEFT);
    int rt = pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (up) b |= 1u << GP2X_UP;
    if (dn) b |= 1u << GP2X_DOWN;
    if (lf) b |= 1u << GP2X_LEFT;
    if (rt) b |= 1u << GP2X_RIGHT;
    if (up && lf) b |= 1u << GP2X_UPLEFT;
    if (up && rt) b |= 1u << GP2X_UPRIGHT;
    if (dn && lf) b |= 1u << GP2X_DOWNLEFT;
    if (dn && rt) b |= 1u << GP2X_DOWNRIGHT;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_START))  b |= 1u << GP2X_START;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_SELECT)) b |= 1u << GP2X_SELECT;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_L))      b |= 1u << GP2X_L;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_R))      b |= 1u << GP2X_R;
    /* RetroPad face positions: bottom/right/left/top -> GP2X A/B/X/Y. */
    if (pressed(RETRO_DEVICE_ID_JOYPAD_B)) b |= 1u << GP2X_A;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_A)) b |= 1u << GP2X_B;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_Y)) b |= 1u << GP2X_X;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_X)) b |= 1u << GP2X_Y;
    if (pressed(RETRO_DEVICE_ID_JOYPAD_L3)) b |= 1u << GP2X_CLICK;
    g_shm->buttons = b;

    int16_t px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    int16_t py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    int down = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
    unsigned w = g_shm->width ? g_shm->width : 320;
    unsigned h = g_shm->height ? g_shm->height : 240;
    int x = (int)(((int64_t)px + 32768) * w / 65536);
    int y = (int)(((int64_t)py + 32768) * h / 65536);
    if (x < 0) x = 0;
    if (x >= (int)w) x = (int)w - 1;
    if (y < 0) y = 0;
    if (y >= (int)h) y = (int)h - 1;
    g_shm->touch_x = (int16_t)x;
    g_shm->touch_y = (int16_t)y;
    g_shm->touch_down = (uint32_t)down;
}

static int16_t pcm_sample(const uint8_t *p, unsigned bits, int is_signed) {
    if (bits == 8) return is_signed ? (int16_t)((int8_t)p[0] * 256)
                                    : (int16_t)(((int)p[0] - 128) << 8);
    uint16_t u = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return is_signed ? (int16_t)u : (int16_t)((int)u - 32768);
}

static void drain_audio(void) {
    uint32_t freq = g_shm->audio_freq;
    unsigned ch = g_shm->audio_channels;
    uint32_t fmt = g_shm->audio_format;
    if (!g_shm->audio_active || !freq || (ch != 1 && ch != 2)) return;
    unsigned bits = fmt & 0xffu;
    if (bits != 8 && bits != 16) return;
    unsigned bytes_per = bits / 8;
    unsigned frame_bytes = ch * bytes_per;
    uint32_t avail = g_shm->a_write - g_shm->a_read;
    avail -= avail % frame_bytes;
    /* Keep latency bounded after a slow frontend frame by dropping all but the newest 250ms. */
    uint32_t keep = freq * frame_bytes / 4;
    if (avail > keep) { g_shm->a_read += avail - keep; avail = keep; }

    uint32_t r = g_shm->a_read;
    uint32_t source_frames = avail / frame_bytes;
    size_t out_n = 0;
    int is_signed = (fmt & 0x8000u) != 0;
    for (uint32_t i = 0; i < source_frames && out_n < MAX_AUDIO_OUT; i++) {
        uint8_t raw[4] = {0};
        for (unsigned k = 0; k < frame_bytes; k++)
            raw[k] = g_shm->aring[(r + i * frame_bytes + k) % GP2XSHM_ARING];
        int16_t l = pcm_sample(raw, bits, is_signed);
        int16_t rr = ch == 2 ? pcm_sample(raw + bytes_per, bits, is_signed) : l;
        audio_phase += OUTPUT_RATE;
        while (audio_phase >= freq && out_n < MAX_AUDIO_OUT) {
            audio_phase -= freq;
            audio_out[out_n * 2] = l;
            audio_out[out_n * 2 + 1] = rr;
            out_n++;
        }
    }
    g_shm->a_read = r + source_frames * frame_bytes;
    if (out_n && audio_batch_cb != null_audio_batch) audio_batch_cb(audio_out, out_n);
    else for (size_t i = 0; i < out_n; i++) audio_cb(audio_out[i*2], audio_out[i*2+1]);
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb ? cb : null_video; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb ? cb : null_audio; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb ? cb : null_audio_batch; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb ? cb : null_input_poll; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb ? cb : null_input_state; }

RETRO_API void retro_init(void) {
    video_cb = video_cb ? video_cb : null_video;
    audio_cb = audio_cb ? audio_cb : null_audio;
    audio_batch_cb = audio_batch_cb ? audio_batch_cb : null_audio_batch;
    input_poll_cb = input_poll_cb ? input_poll_cb : null_input_poll;
    input_state_cb = input_state_cb ? input_state_cb : null_input_state;
    if (environ_cb) {
        struct retro_log_callback log = {0};
        if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) log_cb = log.log;
        unsigned level = 5;
        environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
    }
}

RETRO_API void retro_deinit(void);

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof *info);
    info->library_name = "magiceyes";
    info->library_version = ME_VERSION;
    info->valid_extensions = "gpe|zip";
    info->need_fullpath = true;
    info->block_extract = true;  /* magiceyes resolves archives containing auxiliary assets */
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof *info);
    info->geometry.base_width = 320;
    info->geometry.base_height = 240;
    info->geometry.max_width = GP2XSHM_MAXW;
    info->geometry.max_height = GP2XSHM_MAXH;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = OUTPUT_RATE;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    if (!game || !game->path || !*game->path || engine_thread_valid) return false;
    enum retro_pixel_format pf = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pf)) {
        core_log(RETRO_LOG_ERROR, "magiceyes: frontend does not support RGB565\n");
        return false;
    }

    const char *system_dir = NULL, *save_dir = NULL;
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
    environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
    if (!save_dir || !*save_dir) save_dir = system_dir && *system_dir ? system_dir : ".";
    if (!system_dir || !*system_dir) system_dir = ".";

    char sysroot[PATH_MAX], saveroot[PATH_MAX], p[PATH_MAX];
    join_path(sysroot, sizeof sysroot, system_dir, "magiceyes");
    join_path(saveroot, sizeof saveroot, save_dir, "magiceyes");
    me_mkdirs(sysroot); me_mkdirs(saveroot);
    join_path(p, sizeof p, saveroot, "config");   me_paths_set_runtime(ME_PATH_SETTINGS, p);
    join_path(p, sizeof p, sysroot, "firmware"); me_paths_set_runtime(ME_PATH_FIRMWARE, p);
    join_path(p, sizeof p, saveroot, "cache");   me_paths_set_runtime(ME_PATH_CACHE, p);
    me_save_set_base(saveroot);
    join_path(engine_argv0, sizeof engine_argv0, sysroot, "magiceyes_libretro");
    snprintf(content_path, sizeof content_path, "%s", game->path);

    static const struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Stick click"},
        {0, 0, 0, 0, NULL}
    };
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);

    engine_thread_done = 0;
    g_engine_ready = 0;       /* clear the previous session before the new thread is scheduled */
    audio_phase = 0;
    if (pthread_create(&engine_thread, NULL, engine_entry, NULL) != 0) return false;
    engine_thread_valid = 1;
    for (int i = 0; i < 6000 && !engine_thread_done && g_engine_ready == 0; i++) me_usleep(10000);
    if (g_engine_ready != 1) {
        core_log(RETRO_LOG_ERROR, "magiceyes: content failed to load: %s\n", content_path);
        engine_request_shutdown();
        pthread_join(engine_thread, NULL);
        engine_thread_valid = 0;
        shm_shutdown();
        return false;
    }
    monitor_stop = monitor_paused = 0;
    run_heartbeat = 0;
    if (pthread_create(&monitor_thread, NULL, monitor_entry, NULL) != 0) {
        engine_request_shutdown();
        pthread_join(engine_thread, NULL);
        engine_thread_valid = 0;
        shm_shutdown();
        return false;
    }
    core_log(RETRO_LOG_INFO, "magiceyes: loaded %s\n", content_path);
    return true;
}

RETRO_API void retro_unload_game(void) {
    if (!engine_thread_valid) return;
    monitor_stop = 1;
    pthread_join(monitor_thread, NULL);
    engine_request_shutdown();
    pthread_join(engine_thread, NULL);
    shm_shutdown();
    engine_thread_valid = 0;
    engine_thread_done = 0;
    audio_phase = 0;
}

RETRO_API void retro_deinit(void) { retro_unload_game(); }

RETRO_API void retro_reset(void) {
    if (engine_thread_valid && content_path[0]) engine_request_reload(content_path);
}

RETRO_API void retro_run(void) {
    input_poll_cb();
    gp2x_shm_t *shm = g_shm;
    if (!engine_thread_valid || !shm) {
        video_cb(NULL, 320, 240, 320 * 2);
        return;
    }
    run_heartbeat++;
    if (monitor_paused) { dbg_resume(); monitor_paused = 0; }
    update_input();
    shm->viewer_heartbeat++;
    unsigned w = shm->width ? shm->width : 320;
    unsigned h = shm->height ? shm->height : 240;
    if (w > GP2XSHM_MAXW) w = GP2XSHM_MAXW;
    if (h > GP2XSHM_MAXH) h = GP2XSHM_MAXH;
    video_cb(shm->pixels, w, h, GP2XSHM_MAXW * 2);
    drain_audio();
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port; (void)device;
}
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index; (void)enabled; (void)code;
}
RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t n) {
    (void)type; (void)info; (void)n; return false;
}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
