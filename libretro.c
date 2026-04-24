#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "i8080.h"

#define VIDEO_W 320
#define VIDEO_H 240

static i8080 cpu;
static uint32_t *video_buffer;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_init(void) {
    video_buffer = malloc(VIDEO_W * VIDEO_H * sizeof(uint32_t));
    i8080_reset(&cpu);
}

void retro_deinit(void) {
    free(video_buffer);
}

void retro_get_system_info(struct retro_system_info *info) {
    info->library_name = "Altair8800";
    info->library_version = "1.0";
    info->need_fullpath = false;
    info->valid_extensions = "bin|hex|dsk|cas";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;
    info->geometry.base_width = VIDEO_W;
    info->geometry.base_height = VIDEO_H;
    info->geometry.max_width = VIDEO_W;
    info->geometry.max_height = VIDEO_H;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}

bool retro_load_game(const struct retro_game_info *game) {
    if (!game) return false;
    memcpy(cpu.memory, game->data, game->size);
    return true;
}

void retro_run(void) {
    input_poll_cb();
    for(int i = 0; i < 33333; i++) i8080_step(&cpu);
    memset(video_buffer, 0, VIDEO_W * VIDEO_H * sizeof(uint32_t));
    for(int i = 0; i < 16; i++) {
        if((cpu.pc >> i) & 1) video_buffer[10 + i*6 + (20 * VIDEO_W)] = 0xFF0000;
    }
    video_cb(video_buffer, VIDEO_W, VIDEO_H, VIDEO_W * sizeof(uint32_t));
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_reset(void) { i8080_reset(&cpu); }
size_t retro_serialize_size(void) { return sizeof(cpu); }
bool retro_serialize(void *data, size_t size) { memcpy(data, &cpu, size); return true; }
bool retro_unserialize(const void *data, size_t size) { memcpy(&cpu, data, size); return true; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned port, unsigned device) {}
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) { return false; }
void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {}