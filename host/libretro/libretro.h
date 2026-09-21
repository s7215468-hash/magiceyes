/* Minimal libretro ABI declarations used by magiceyes.
 * The libretro API is MIT licensed; canonical header:
 * https://github.com/libretro/RetroArch/blob/master/libretro-common/include/libretro.h */
#ifndef MAGICEYES_LIBRETRO_H
#define MAGICEYES_LIBRETRO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#define RETRO_API __declspec(dllexport)
#define RETRO_CALLCONV __cdecl
#else
#define RETRO_API __attribute__((visibility("default")))
#define RETRO_CALLCONV
#endif

#define RETRO_API_VERSION 1

typedef bool (RETRO_CALLCONV *retro_environment_t)(unsigned, void *);
typedef void (RETRO_CALLCONV *retro_video_refresh_t)(const void *, unsigned, unsigned, size_t);
typedef void (RETRO_CALLCONV *retro_audio_sample_t)(int16_t, int16_t);
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_t)(const int16_t *, size_t);
typedef void (RETRO_CALLCONV *retro_input_poll_t)(void);
typedef int16_t (RETRO_CALLCONV *retro_input_state_t)(unsigned, unsigned, unsigned, unsigned);

enum retro_environment_cmd {
    RETRO_ENVIRONMENT_GET_CAN_DUPE = 3,
    RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL = 8,
    RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9,
    RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10,
    RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS = 11,
    RETRO_ENVIRONMENT_GET_VARIABLE = 15,
    RETRO_ENVIRONMENT_SET_VARIABLES = 16,
    RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE = 17,
    RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27,
    RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31,
    RETRO_ENVIRONMENT_SET_CONTROLLER_INFO = 35,
    RETRO_ENVIRONMENT_SET_GEOMETRY = 37,
    RETRO_ENVIRONMENT_GET_FASTFORWARDING = 49,
    RETRO_ENVIRONMENT_GET_INPUT_BITMASKS = 51
};

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565 = 2
};

enum retro_device {
    RETRO_DEVICE_NONE = 0,
    RETRO_DEVICE_JOYPAD = 1,
    RETRO_DEVICE_MOUSE = 2,
    RETRO_DEVICE_KEYBOARD = 3,
    RETRO_DEVICE_LIGHTGUN = 4,
    RETRO_DEVICE_ANALOG = 5,
    RETRO_DEVICE_POINTER = 6
};

enum retro_device_id_joypad {
    RETRO_DEVICE_ID_JOYPAD_B = 0,
    RETRO_DEVICE_ID_JOYPAD_Y = 1,
    RETRO_DEVICE_ID_JOYPAD_SELECT = 2,
    RETRO_DEVICE_ID_JOYPAD_START = 3,
    RETRO_DEVICE_ID_JOYPAD_UP = 4,
    RETRO_DEVICE_ID_JOYPAD_DOWN = 5,
    RETRO_DEVICE_ID_JOYPAD_LEFT = 6,
    RETRO_DEVICE_ID_JOYPAD_RIGHT = 7,
    RETRO_DEVICE_ID_JOYPAD_A = 8,
    RETRO_DEVICE_ID_JOYPAD_X = 9,
    RETRO_DEVICE_ID_JOYPAD_L = 10,
    RETRO_DEVICE_ID_JOYPAD_R = 11,
    RETRO_DEVICE_ID_JOYPAD_L2 = 12,
    RETRO_DEVICE_ID_JOYPAD_R2 = 13,
    RETRO_DEVICE_ID_JOYPAD_L3 = 14,
    RETRO_DEVICE_ID_JOYPAD_R3 = 15,
    RETRO_DEVICE_ID_JOYPAD_MASK = 256
};

enum retro_device_id_pointer {
    RETRO_DEVICE_ID_POINTER_X = 0,
    RETRO_DEVICE_ID_POINTER_Y = 1,
    RETRO_DEVICE_ID_POINTER_PRESSED = 2
};

enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
    RETRO_LOG_DUMMY = INT32_MAX
};

typedef void (RETRO_CALLCONV *retro_log_printf_t)(enum retro_log_level, const char *, ...);
struct retro_log_callback { retro_log_printf_t log; };

struct retro_game_geometry {
    unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio;
};
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};
struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    bool need_fullpath;
    bool block_extract;
};
struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};
struct retro_input_descriptor {
    unsigned port, device, index, id;
    const char *description;
};
struct retro_variable {
    const char *key;
    const char *value;
};
struct retro_controller_description {
    const char *desc;
    unsigned id;
};
struct retro_controller_info {
    const struct retro_controller_description *types;
    unsigned num_types;
};
struct retro_game_info_ext;

#define RETRO_REGION_NTSC 0
#define RETRO_MEMORY_SAVE_RAM 0
#define RETRO_MEMORY_RTC 1
#define RETRO_MEMORY_SYSTEM_RAM 2
#define RETRO_MEMORY_VIDEO_RAM 3

#endif
