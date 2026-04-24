#include "libretro.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    uint8_t a, b, c, d, e, h, l, f;
    uint16_t pc, sp;
    uint8_t mem[65536];
} i8080_t;

static i8080_t cpu;
static uint32_t frame_buf[320 * 240];
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_environment_t env_cb;

void retro_set_environment(retro_environment_t cb) {
    env_cb = cb;
    bool no_rom = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

void retro_init(void) {}
void retro_deinit(void) {}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Altair 8800";
    info->library_version = "v1.0.7";
    info->need_fullpath = false;
    info->valid_extensions = "bin|hex|alt";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
    info->geometry.base_width = 320;
    info->geometry.base_height = 240;
    info->geometry.max_width = 320;
    info->geometry.max_height = 240;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}

bool retro_load_game(const struct retro_game_info *game) {
    memset(&cpu, 0, sizeof(cpu));
    if (game && game->data && game->size > 0) {
        size_t s = (game->size > 65536) ? 65536 : game->size;
        memcpy(cpu.mem, game->data, s);
    }
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    return true;
}

void retro_run(void) {
    if (poll_cb) poll_cb();
    
    for (int i = 0; i < 1000; i++) {
        uint8_t op = cpu.mem[cpu.pc];
        if (op == 0xC3) {
            uint16_t target = cpu.mem[(cpu.pc+1)&0xFFFF] | (cpu.mem[(cpu.pc+2)&0xFFFF] << 8);
            cpu.pc = target;
        } else {
            cpu.pc = (cpu.pc + 1) & 0xFFFF;
        }
    }
    
    if (video_cb) {
        memset(frame_buf, 0, sizeof(frame_buf));
        for (int i = 0; i < 16; i++) {
            if ((cpu.pc >> i) & 1) {
                int x_start = 20 + (i * 18);
                for (int y = 110; y < 125; y++) {
                    for (int x = x_start; x < x_start + 12; x++) {
                        frame_buf[y * 320 + x] = 0xFFFF0000;
                    }
                }
            }
        }
        video_cb(frame_buf, 320, 240, sizeof(frame_buf[0]) * 320);
    }
}

void retro_unload_game(void) {}
void retro_reset(void) { cpu.pc = 0; }
size_t retro_serialize_size(void) { return sizeof(cpu); }
bool retro_serialize(void *data, size_t size) { if (size >= sizeof(cpu)) { memcpy(data, &cpu, sizeof(cpu)); return true; } return false; }
bool retro_unserialize(const void *data, size_t size) { if (size >= sizeof(cpu)) { memcpy(&cpu, data, sizeof(cpu)); return true; } return false; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { (void)type; (void)info; (void)num; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
