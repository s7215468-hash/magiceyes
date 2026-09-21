/* magiceyes libretro adapter.
 *
 * The main ARM CPU, guest workers and helper presenter keep the engine's existing threading
 * model. In the default synchronized mode retro_run grants them one 60 Hz wall-time slice and
 * parks them cooperatively before returning; asynchronous mode preserves the original free-run
 * model. The frontend owns all host video, audio and input APIs in either mode. */
#include "libretro.h"
#include "audio.h"
#include "engine.h"
#include "paths.h"

#ifndef ME_VERSION
#define ME_VERSION "0.2.0-dev"
#endif

#define FRAME_SECONDS (1.0 / 60.0)
#define FASTFORWARD_RATE 4.0
#define FASTFORWARD_BUDGET (FRAME_SECONDS / FASTFORWARD_RATE)
/* libretro requires a stable size for the whole content session. The emulated machines expose
   at most 64MB of physical RAM in the currently-supported paths; the compressed MST plus CPU,
   device and adapter records fit below this with headroom. Oversize states fail explicitly. */
#define LIBRETRO_STATE_BYTES (96u * 1024u * 1024u)
#define LIBRETRO_STATE_HEADER 32u
#define LIBRETRO_STATE_VERSION 2u

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
static volatile int monitor_stop, core_paused;
static volatile uint64_t run_heartbeat;
static char content_path[PATH_MAX];
static char engine_argv0[PATH_MAX];
static char state_path[PATH_MAX];
static me_lr_audio_t audio_state;
static double paused_at;
static int timing_synchronized = 1;
static volatile int fastforward_active;
static int pointer_enabled = 1;
static int controller_enabled = 1;
static unsigned video_width, video_height;

static const struct retro_variable core_variables[] = {
    {"magiceyes_timing", "Timing model; Synchronized|Asynchronous"},
    {"magiceyes_pointer", "Pointer/touch input; Enabled|Disabled"},
    {NULL, NULL}
};

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

static int path_has_leaf(const char *path, const char *leaf) {
    if (!path || !leaf) return 0;
    size_t n = strlen(path);
    while (n && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    size_t l = strlen(leaf);
    if (n < l || (n > l && path[n-l-1] != '/' && path[n-l-1] != '\\')) return 0;
#ifdef _WIN32
    return !_strnicmp(path + n - l, leaf, l);
#else
    return !strncasecmp(path + n - l, leaf, l);
#endif
}

static void update_variables(void) {
    if (!environ_cb) return;
    struct retro_variable v = {"magiceyes_timing", NULL};
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        timing_synchronized = strcmp(v.value, "Asynchronous") != 0;
    v.key = "magiceyes_pointer"; v.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        pointer_enabled = strcmp(v.value, "Disabled") != 0;
}

static int pause_guest(int quiesce) {
    if (core_paused) return 1;
    int rc = quiesce ? dbg_quiesce(4000, NULL, NULL, NULL)
                     : dbg_pause(1000, NULL, NULL, NULL);
    if (rc != 0) return 0;
    core_paused = 1;
    paused_at = host_now();
    return 1;
}

static void resume_guest(void) {
    if (!core_paused) return;
    /* Guest-observable clocks stop between frontend callbacks. This is what makes frame advance
       advance one slice instead of including however long the frontend menu stayed open. */
    guest_clock_adjust(-(host_now() - paused_at) * guest_clock_rate());
    core_paused = 0;
    dbg_resume();
}

static void leave_fastforward(void) {
    if (!fastforward_active) return;
    if (!core_paused && !pause_guest(0)) return;
    guest_clock_set_rate(1.0);
    fastforward_active = 0;
    core_log(RETRO_LOG_INFO, "magiceyes: fast-forward disabled\n");
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
        if ((!timing_synchronized || fastforward_active) && seen && !core_paused &&
            now - changed > 0.12) {
            pause_guest(0);
        }
        me_usleep(10000);
    }
    resume_guest();
    return NULL;
}

static int pressed(unsigned id) {
    return controller_enabled && input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
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

    int16_t px = pointer_enabled ? input_state_cb(0, RETRO_DEVICE_POINTER, 0,
                                                  RETRO_DEVICE_ID_POINTER_X) : 0;
    int16_t py = pointer_enabled ? input_state_cb(0, RETRO_DEVICE_POINTER, 0,
                                                  RETRO_DEVICE_ID_POINTER_Y) : 0;
    int down = pointer_enabled &&
        input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
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

static void drain_audio(void) {
    size_t pending = me_lr_audio_prepare(&audio_state, g_shm->aring, GP2XSHM_ARING,
                                         &g_shm->a_read, g_shm->a_write,
                                         g_shm->audio_active, g_shm->audio_freq,
                                         g_shm->audio_format, g_shm->audio_channels);
    if (!pending) return;
    const int16_t *out = me_lr_audio_output(&audio_state);
    if (audio_batch_cb != null_audio_batch) {
        size_t used = audio_batch_cb(out, pending);
        me_lr_audio_consume(&audio_state, used);
    } else {
        for (size_t i = 0; i < pending; i++) audio_cb(out[i*2], out[i*2+1]);
        me_lr_audio_consume(&audio_state, pending);
    }
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)core_variables);
}
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
        static const struct retro_controller_description controllers[] = {
            {"RetroPad", RETRO_DEVICE_JOYPAD}
        };
        static const struct retro_controller_info ports[] = {
            {controllers, 1}, {NULL, 0}
        };
        environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
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
    info->timing.sample_rate = ME_LR_AUDIO_OUTPUT_RATE;
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
    if (path_has_leaf(save_dir, "magiceyes")) snprintf(saveroot, sizeof saveroot, "%s", save_dir);
    else join_path(saveroot, sizeof saveroot, save_dir, "magiceyes");
    me_mkdirs(sysroot); me_mkdirs(saveroot);
    join_path(p, sizeof p, saveroot, "config");   me_paths_set_runtime(ME_PATH_SETTINGS, p);
    join_path(p, sizeof p, sysroot, "firmware"); me_paths_set_runtime(ME_PATH_FIRMWARE, p);
    join_path(p, sizeof p, saveroot, "cache");   me_paths_set_runtime(ME_PATH_CACHE, p);
    me_save_set_base(saveroot);
    int state_n = snprintf(state_path, sizeof state_path, "%s/.libretro-state-%ld.mst",
                           p, (long)getpid());
    if (state_n < 0 || (size_t)state_n >= sizeof state_path) {
        core_log(RETRO_LOG_ERROR, "magiceyes: save directory path is too long\n");
        state_path[0] = 0;
        return false;
    }
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
    guest_clock_reset();
    me_lr_audio_reset(&audio_state);
    video_width = video_height = 0;
    update_variables();
    dbg_set_quantum(timing_synchronized ? 50000 : 0);
    if (pthread_create(&engine_thread, NULL, engine_entry, NULL) != 0) {
        dbg_set_quantum(0);
        return false;
    }
    engine_thread_valid = 1;
    for (int i = 0; i < 6000 && !engine_thread_done && g_engine_ready == 0; i++) me_usleep(10000);
    if (g_engine_ready != 1) {
        core_log(RETRO_LOG_ERROR, "magiceyes: content failed to load: %s\n", content_path);
        engine_request_shutdown();
        pthread_join(engine_thread, NULL);
        engine_thread_valid = 0;
        shm_shutdown();
        dbg_set_quantum(0);
        return false;
    }
    monitor_stop = core_paused = fastforward_active = 0;
    run_heartbeat = 0;
    if (timing_synchronized && !pause_guest(1)) {
        engine_request_shutdown();
        pthread_join(engine_thread, NULL);
        engine_thread_valid = 0;
        shm_shutdown();
        dbg_set_quantum(0);
        return false;
    }
    if (pthread_create(&monitor_thread, NULL, monitor_entry, NULL) != 0) {
        engine_request_shutdown();
        pthread_join(engine_thread, NULL);
        engine_thread_valid = 0;
        shm_shutdown();
        dbg_set_quantum(0);
        return false;
    }
    core_log(RETRO_LOG_INFO, "magiceyes: loaded %s\n", content_path);
    return true;
}

RETRO_API void retro_unload_game(void) {
    if (!engine_thread_valid) return;
    leave_fastforward();
    monitor_stop = 1;
    pthread_join(monitor_thread, NULL);
    engine_request_shutdown();
    pthread_join(engine_thread, NULL);
    shm_shutdown();
    engine_thread_valid = 0;
    engine_thread_done = 0;
    me_lr_audio_reset(&audio_state);
    fastforward_active = 0;
    guest_clock_reset();
    dbg_set_quantum(0);
    if (state_path[0]) remove(state_path);
    state_path[0] = 0;
}

RETRO_API void retro_deinit(void) { retro_unload_game(); }

RETRO_API void retro_reset(void) {
    if (!engine_thread_valid || !content_path[0]) return;
    leave_fastforward();
    resume_guest();
    me_lr_audio_reset(&audio_state);
    g_engine_ready = 0;
    engine_request_reload(content_path);
    for (int i = 0; i < 6000 && !engine_thread_done && g_engine_ready == 0; i++) me_usleep(10000);
    if (g_engine_ready == 1 && timing_synchronized) pause_guest(1);
}

RETRO_API void retro_run(void) {
    input_poll_cb();
    gp2x_shm_t *shm = g_shm;
    if (!engine_thread_valid || !shm) {
        video_cb(NULL, 320, 240, 320 * 2);
        return;
    }
    run_heartbeat++;
    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        int was_synchronized = timing_synchronized;
        update_variables();
        if (timing_synchronized != was_synchronized) {
            if (timing_synchronized) {
                /* The currently-running indefinite uc call needs one ordinary debugger kick;
                   subsequent frame boundaries use safe instruction-count yields. */
                if (!pause_guest(0)) timing_synchronized = 0;
                else dbg_set_quantum(50000);
            } else {
                leave_fastforward();
                dbg_set_quantum(0);
                resume_guest();
            }
        }
    }
    if (!controller_enabled) g_shm->buttons = 0;
    update_input();
    shm->viewer_heartbeat++;
    if (timing_synchronized) {
        bool fastforward = false;
        if (environ_cb) environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &fastforward);
        if (fastforward) {
            if (!fastforward_active) {
                if (!core_paused) pause_guest(0);
                if (core_paused) {
                    guest_clock_set_rate(FASTFORWARD_RATE);
                    fastforward_active = 1;
                    core_log(RETRO_LOG_INFO, "magiceyes: fast-forward enabled (%.0fx)\n",
                             FASTFORWARD_RATE);
                }
            }
        } else leave_fastforward();
        if (fastforward_active) {
            /* Keep the CPUs running between callbacks. Parking every 4 ms starves titles with
               several guest threads and turns fast-forward into repeated partial CPU slices.
               The heartbeat watchdog parks them if callbacks stop (menu/pause). */
            resume_guest();
            present_uncap();
            double until = host_now() + FASTFORWARD_BUDGET;
            while (!engine_thread_done && host_now() < until) me_usleep(250);
        } else {
            if (!core_paused) pause_guest(0);
            resume_guest();
            double started = host_now();
            double until = started + FRAME_SECONDS;
            while (!engine_thread_done && host_now() < until) me_usleep(500);
            if (pause_guest(0)) {
                /* Correct scheduler/parking latency so one callback is exactly one guest frame. */
                guest_clock_adjust(FRAME_SECONDS - (paused_at - started));
            }
        }
    } else {
        resume_guest();
    }
    unsigned w = shm->width ? shm->width : 320;
    unsigned h = shm->height ? shm->height : 240;
    if (w > GP2XSHM_MAXW) w = GP2XSHM_MAXW;
    if (h > GP2XSHM_MAXH) h = GP2XSHM_MAXH;
    if (w != video_width || h != video_height) {
        struct retro_game_geometry geometry = {
            w, h, GP2XSHM_MAXW, GP2XSHM_MAXH, h ? (float)w / (float)h : 4.0f / 3.0f
        };
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
        video_width = w; video_height = h;
    }
    video_cb(shm->pixels, w, h, GP2XSHM_MAXW * 2);
    drain_audio();
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port != 0) return;
    if (device == RETRO_DEVICE_NONE) controller_enabled = 0;
    else if (device == RETRO_DEVICE_JOYPAD) controller_enabled = 1;
    else {
        controller_enabled = 0;
        core_log(RETRO_LOG_WARN, "magiceyes: unsupported controller device %u on port 0\n", device);
    }
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
RETRO_API size_t retro_serialize_size(void) { return LIBRETRO_STATE_BYTES; }

RETRO_API bool retro_serialize(void *data, size_t size) {
    if (!data || size < LIBRETRO_STATE_BYTES || !engine_thread_valid || !state_path[0]) return false;
    char err[192] = {0};
    if (me_state_save(state_path, err, sizeof err) != 0) {
        core_log(RETRO_LOG_ERROR,
                 "magiceyes: serialize failed: %s (done=%d ready=%d game='%s' threads=%d uc=%p)\n",
                 err, engine_thread_done, g_engine_ready, g_cur_game, g_nth,
                 g_nth > 0 ? (void *)g_th[0].uc : NULL);
        return false;
    }
    FILE *f = fopen(state_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); remove(state_path); return false; }
    long n = ftell(f);
    if (n < 0 || (size_t)n + LIBRETRO_STATE_HEADER > LIBRETRO_STATE_BYTES) {
        fclose(f); remove(state_path);
        core_log(RETRO_LOG_ERROR, "magiceyes: state exceeds the %u MiB libretro limit\n",
                 LIBRETRO_STATE_BYTES / (1024u * 1024u));
        return false;
    }
    rewind(f);
    uint8_t *out = data;
    memset(out, 0, LIBRETRO_STATE_BYTES);
    memcpy(out, "MELRSTA", 7);
    put_u32(out + 8, LIBRETRO_STATE_VERSION);
    put_u32(out + 12, (uint32_t)n);
    /* Version 2 explicitly flushes frontend/source audio on load. Bytes 16..31 stay reserved;
       version 1 used them for resampler phase and a pending frontend batch. */
    size_t off = LIBRETRO_STATE_HEADER;
    bool ok = fread(out + off, 1, (size_t)n, f) == (size_t)n;
    fclose(f); remove(state_path);
    return ok;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    if (!data || size < LIBRETRO_STATE_HEADER || !engine_thread_valid || !state_path[0]) return false;
    const uint8_t *in = data;
    uint32_t version = get_u32(in + 8);
    if (memcmp(in, "MELRSTA", 7) || (version != 1 && version != LIBRETRO_STATE_VERSION))
        return false;
    uint32_t n = get_u32(in + 12), pending = get_u32(in + 24);
    if (version == LIBRETRO_STATE_VERSION && pending != 0) return false;
    size_t off = LIBRETRO_STATE_HEADER + (size_t)pending * 4;
    if (pending > ME_LR_AUDIO_MAX_OUTPUT || off > size || n > size - off) return false;
    FILE *f = fopen(state_path, "wb");
    int write_ok = f && fwrite(in + off, 1, n, f) == n;
    if (f && fclose(f) != 0) write_ok = 0;
    if (!write_ok) {
        remove(state_path);
        return false;
    }
    char err[192] = {0};
    uint32_t epoch = g_state_epoch;
    g_state_pause_after_restore = 1;
    core_paused = 0;                 /* request_restore force-resumes the old machine */
    if (me_state_request_restore(state_path, err, sizeof err) != 0) {
        g_state_pause_after_restore = 0;
        remove(state_path);
        core_log(RETRO_LOG_ERROR, "magiceyes: unserialize refused: %s\n", err);
        return false;
    }
    double deadline = host_now() + 15.0;
    while (!engine_thread_done && g_engine_ready != -1 && g_state_epoch == epoch &&
           host_now() < deadline) me_usleep(1000);
    remove(state_path);
    if (g_state_epoch == epoch || !pause_guest(1)) {
        core_log(RETRO_LOG_ERROR, "magiceyes: unserialize did not complete\n");
        return false;
    }
    /* Device restore discards queued ring PCM. Discard version 1's pending frontend batch too:
       it belongs to the abandoned timeline. The restored guest will establish a fresh cushion. */
    me_lr_audio_reset(&audio_state);
    return true;
}
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
