#include "libretro.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t a, b, c, d, e, h, l, f;
    uint16_t pc, sp;
    uint8_t mem[65536];
} i8080_t;

static i8080_t core_cpu;
static uint32_t *core_video = NULL;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;

void retro_set_environment(retro_environment_t cb) { (void)cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

void retro_init(void) {
    core_video = (uint32_t*)calloc(320 * 240, sizeof(uint32_t));
    memset(&core_cpu, 0, sizeof(core_cpu));
}

void retro_deinit(void) {
    if (core_video) free(core_video);
    core_video = NULL;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "Altair 8800";
    info->library_version = "1.0.3";
    info->need_fullpath = false;
    info->valid_extensions = "alt|bin|hex";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
    info->geometry.base_width = 320;
    info->geometry.base_height = 240;
    info->geometry.max_width = 320;
    info->geometry.max_height = 240;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}

bool retro_load_game(const struct retro_game_info *game) {
    memset(&core_cpu, 0, sizeof(core_cpu));
    if (game && game->data && game->size > 0) {
        size_t s = (game->size > 65536) ? 65536 : (size_t)game->size;
        memcpy(core_cpu.mem, game->data, s);
    }
    return true;
}

void retro_run(void) {
    if (poll_cb) poll_cb();

    for (int i = 0; i < 2000; i++) {
        uint8_t op = core_cpu.mem[core_cpu.pc];
        if (op == 0xC3) {
            uint16_t low = core_cpu.mem[(core_cpu.pc + 1) & 0xFFFF];
            uint16_t high = core_cpu.mem[(core_cpu.pc + 2) & 0xFFFF];
            core_cpu.pc = (uint16_t)(low | (high << 8));
        } else {
            core_cpu.pc = (uint16_t)((core_cpu.pc + 1) & 0xFFFF);
        }
    }

    if (core_video && video_cb) {
        memset(core_video, 0, 320 * 240 * sizeof(uint32_t));
        for (int i = 0; i < 16; i++) {
            if ((core_cpu.pc >> i) & 1) {
                uint32_t color = 0xFFFF0000;
                int start_x = 20 + (i * 18);
                for (int y = 110; y < 125; y++) {
                    for (int x = start_x; x < start_x + 12; x++) {
                        core_video[y * 320 + x] = color;
                    }
                }
            }
        }
        video_cb(core_video, 320, 240, 320 * sizeof(uint32_t));
    }
}

void retro_unload_game(void) {}
void retro_reset(void) { memset(&core_cpu, 0, sizeof(core_cpu)); }
size_t retro_serialize_size(void) { return sizeof(core_cpu); }
bool retro_serialize(void *data, size_t size) { if (size >= sizeof(core_cpu)) { memcpy(data, &core_cpu, sizeof(core_cpu)); return true; } return false; }
bool retro_unserialize(const void *data, size_t size) { if (size >= sizeof(core_cpu)) { memcpy(&core_cpu, data, sizeof(core_cpu)); return true; } return false; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { (void)type; (void)info; (void)num; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
