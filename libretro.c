#include "libretro.h"
#include "i8080.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* ---------------------------------------------------------------
   Display: 256x256 monochrome framebuffer mapped to XRGB8888.
   The i8080 Space Invaders VRAM lives at 0x2400..0x3FFF (6 KB).
   Each byte = 8 pixels, LSB = top pixel.
   --------------------------------------------------------------- */
#define FB_W  256
#define FB_H  256

static State8080       *cpu        = NULL;
static uint32_t         frame_buf[FB_W * FB_H];

/* Interrupt timing: 8080 ran at ~2 MHz, 60 fps → ~33333 cycles/frame.
   Two RST interrupts per frame (mid-screen = RST 1, vblank = RST 2). */
#define CYCLES_PER_FRAME  33333
#define CYCLES_PER_HALF   (CYCLES_PER_FRAME / 2)

static int half_frame = 0;   /* unused directly, reserved for future use */

/* --- libretro callbacks ---------------------------------------- */
static retro_video_refresh_t      video_cb;
static retro_input_poll_t         poll_cb;
static retro_input_state_t        input_cb;
static retro_environment_t        env_cb;
static retro_audio_sample_batch_t audio_batch_cb;

/* ---------------------------------------------------------------
   I/O port handling – Space Invaders compatible layout.
   Port 0/1/2 = inputs; ports 2/4 = shift register hardware.
   --------------------------------------------------------------- */
static uint8_t  port1        = 0x00;
static uint8_t  port2        = 0x00;
static uint16_t shift_reg    = 0;
static uint8_t  shift_amount = 0;

static uint8_t MachineIN(uint8_t port)
{
    switch (port) {
        case 0: return 0x0e;
        case 1: return port1;
        case 2: return port2;
        case 3: return (uint8_t)(shift_reg >> (8 - shift_amount));
        default: return 0;
    }
}

static void MachineOUT(uint8_t port, uint8_t value)
{
    switch (port) {
        case 2: shift_amount = value & 0x07; break;
        case 4:
            shift_reg = (shift_reg >> 8) | ((uint16_t)value << 8);
            break;
        default: break;
    }
}

/* ---------------------------------------------------------------
   libretro API
   --------------------------------------------------------------- */

void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    bool no_rom = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);
}

void retro_set_video_refresh(retro_video_refresh_t cb)            { video_cb       = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)              { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)  { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                   { poll_cb        = cb; }
void retro_set_input_state(retro_input_state_t cb)                 { input_cb       = cb; }

void retro_init(void)
{
    cpu = Init8080();
}

void retro_deinit(void)
{
    if (cpu) {
        free(cpu->memory);
        free(cpu);
        cpu = NULL;
    }
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "i8080";
    info->library_version  = "v1.0.0";
    info->need_fullpath    = false;
    info->valid_extensions = "bin|hex|com|rom";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 44100.0;
    info->geometry.base_width   = FB_W;
    info->geometry.base_height  = FB_H;
    info->geometry.max_width    = FB_W;
    info->geometry.max_height   = FB_H;
    info->geometry.aspect_ratio = 1.0f;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!cpu) cpu = Init8080();

    memset(cpu->memory, 0, 0x10000);
    cpu->pc         = 0;
    cpu->sp         = 0xF000;
    cpu->int_enable = 0;
    half_frame      = 0;
    port1           = 0;
    port2           = 0;
    shift_reg       = 0;
    shift_amount    = 0;

    if (game && game->data && game->size > 0) {
        size_t sz = (game->size > 0x10000) ? 0x10000 : game->size;
        memcpy(cpu->memory, game->data, sz);
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    return true;
}

/* Poll joypad and map to Space Invaders port 1 layout */
static void UpdateInputs(void)
{
    if (!poll_cb || !input_cb) return;
    poll_cb();

    port1 = 0;

    /* Coin – Select */
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
        port1 |= (1 << 0);
    /* P1 Start */
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
        port1 |= (1 << 2);
    /* Shoot */
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) ||
        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
        port1 |= (1 << 4);
    /* Left */
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
        port1 |= (1 << 5);
    /* Right */
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
        port1 |= (1 << 6);
}

/* Run a half-frame worth of CPU cycles, handling IN/OUT inline */
static void RunCycles(int target)
{
    int ran = 0;
    while (ran < target) {
        uint8_t op = cpu->memory[cpu->pc & 0xFFFF];

        if (op == 0xDB) {                          /* IN port */
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            cpu->a  = MachineIN(port);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;
        } else if (op == 0xD3) {                   /* OUT port */
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            MachineOUT(port, cpu->a);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;
        } else {
            ran += Emulate(cpu, false);
        }
    }
}

/* Render VRAM (0x2400–0x3FFF) to frame_buf as 1-bit monochrome */
static void RenderFrame(void)
{
    const uint8_t *vram = &cpu->memory[0x2400];

    for (int i = 0; i < (FB_W * FB_H / 8); i++) {
        uint8_t byte  = vram[i];
        int     px    = i * 8;

        for (int bit = 0; bit < 8; bit++) {
            int x = (px + bit) % FB_W;
            int y = (px + bit) / FB_W;
            if (x < FB_W && y < FB_H) {
                frame_buf[y * FB_W + x] =
                    (byte & (1 << bit)) ? 0xFFFFFFFF : 0xFF000000;
            }
        }
    }
}

void retro_run(void)
{
    UpdateInputs();

    /* First half-frame */
    RunCycles(CYCLES_PER_HALF);

    /* Mid-screen interrupt – RST 1 */
    if (cpu->int_enable)
        GenInterrupt(cpu, 1);

    /* Second half-frame */
    RunCycles(CYCLES_PER_HALF);

    /* VBlank interrupt – RST 2 */
    if (cpu->int_enable)
        GenInterrupt(cpu, 2);

    /* Render VRAM to framebuffer */
    RenderFrame();

    if (video_cb)
        video_cb(frame_buf, FB_W, FB_H, sizeof(frame_buf[0]) * FB_W);
}

void retro_unload_game(void)
{
    if (cpu) {
        memset(cpu->memory, 0, 0x10000);
        cpu->pc = 0;
    }
}

void retro_reset(void)
{
    if (cpu) {
        cpu->pc         = 0;
        cpu->sp         = 0xF000;
        cpu->int_enable = 0;
        half_frame      = 0;
        port1           = 0;
        port2           = 0;
        shift_reg       = 0;
        shift_amount    = 0;
    }
}

size_t retro_serialize_size(void)
{
    /* CPU struct (memory pointer replaced with NULL) + 64 KB RAM */
    return sizeof(State8080) + 0x10000;
}

bool retro_serialize(void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    uint8_t  *p   = (uint8_t *)data;
    State8080  tmp = *cpu;
    tmp.memory = NULL;                   /* don't serialise host pointer */
    memcpy(p, &tmp, sizeof(State8080));
    p += sizeof(State8080);
    memcpy(p, cpu->memory, 0x10000);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    const uint8_t *p        = (const uint8_t *)data;
    uint8_t       *saved    = cpu->memory;
    memcpy(cpu, p, sizeof(State8080));
    cpu->memory = saved;                 /* restore host pointer */
    p += sizeof(State8080);
    memcpy(cpu->memory, p, 0x10000);
    return true;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port; (void)device;
}

bool retro_load_game_special(unsigned type,
                              const struct retro_game_info *info,
                              size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
    if (!cpu) return NULL;
    if (id == RETRO_MEMORY_SYSTEM_RAM) return cpu->memory;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM) return 0x10000;
    return 0;
}
