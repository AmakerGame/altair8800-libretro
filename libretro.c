/* =============================================================
   libretro.c – Intel 8080 / Altair 8800 RetroArch core
   =============================================================
   CRASH PROTECTION STRATEGY
   --------------------------
   i8080.c calls exit(1) inside UnimplementedInstruction().
   Calling exit() from inside a shared library kills the entire
   RetroArch process.  We neutralise this with two layers:

   Layer 1 – #define redirect (compile-time)
     Before including i8080.h we redefine exit() to our own
     safe_exit() stub.  Because i8080.c is compiled as part of
     the same translation unit (via #include below), every call
     to exit() inside it resolves to safe_exit() instead.

   Layer 2 – setjmp / longjmp (runtime)
     safe_exit() calls longjmp() back to the guard point set in
     RunCycles().  This unwinds the stack safely and sets the
     cpu_halted flag so we stop executing instructions without
     crashing RetroArch.

   Additionally, printf() output from i8080.c is redirected to
   retro_log so the frontend can display it in its log viewer.
   ============================================================= */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>

#include "libretro.h"

/* ----------------------------------------------------------
   Layer 1: redirect exit() and printf() BEFORE i8080.h is
   included, so every reference inside i8080.c resolves here.
   ---------------------------------------------------------- */
static jmp_buf  cpu_exit_jmp;          /* longjmp target set in RunCycles  */
static bool     cpu_halted  = false;   /* set when safe_exit() is triggered */
static bool     jmp_active  = false;   /* true only while RunCycles is live */

/* Replacement for exit(): log the event and longjmp back to
   the nearest RunCycles() guard, or just set the halt flag if
   no guard is active (e.g. during init). */
static void safe_exit(int code)
{
    cpu_halted = true;
    if (jmp_active)
        longjmp(cpu_exit_jmp, code ? code : 1);
    /* If called outside RunCycles, we simply return – the caller
       (UnimplementedInstruction) will return normally and the
       main loop will notice cpu_halted on the next iteration. */
}

/* Replacement for printf(): forward to retro_log when available,
   otherwise discard (never write to stdout from a shared lib). */
static retro_log_printf_t log_cb = NULL;

static int safe_printf(const char *fmt, ...)
{
    if (!log_cb) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_cb(RETRO_LOG_INFO, "%s", buf);
    return n;
}

/* Macro redirects – must be defined BEFORE i8080.h / i8080.c */
#define exit(c)      safe_exit(c)
#define printf(...)  safe_printf(__VA_ARGS__)

/* Now include the emulator source – macros above apply to it */
#include "i8080.h"

/* Undefine so the rest of libretro.c uses normal libc */
#undef exit
#undef printf

/* =============================================================
   Display configuration
   ============================================================= */
#define FB_W  256
#define FB_H  256

static State8080 *cpu       = NULL;
static uint32_t   frame_buf[FB_W * FB_H];

/* Timing: ~2 MHz CPU, 60 fps → 33 333 cycles per frame.
   Split into two halves so we can fire RST 1 (mid-screen)
   and RST 2 (vblank) at the correct scan-line points. */
#define CYCLES_PER_FRAME  33333
#define CYCLES_PER_HALF   (CYCLES_PER_FRAME / 2)

/* =============================================================
   libretro callbacks
   ============================================================= */
static retro_video_refresh_t      video_cb;
static retro_input_poll_t         poll_cb;
static retro_input_state_t        input_cb;
static retro_environment_t        env_cb;
static retro_audio_sample_batch_t audio_batch_cb;

/* =============================================================
   Space Invaders I/O – shift-register hardware + joypad ports
   ============================================================= */
static uint8_t  port1        = 0x00;   /* P1 controls          */
static uint8_t  port2        = 0x00;   /* P2 / DIP switches    */
static uint16_t shift_reg    = 0;      /* 16-bit shift register */
static uint8_t  shift_amount = 0;      /* shift offset (0-7)   */

/* Read from an 8080 IN port */
static uint8_t MachineIN(uint8_t port)
{
    switch (port) {
        case 0: return 0x0e;   /* always-set hardware bits */
        case 1: return port1;
        case 2: return port2;
        /* Shift register read: return the byte starting at bit (8 - amount) */
        case 3: return (uint8_t)(shift_reg >> (8 - shift_amount));
        default: return 0;
    }
}

/* Write to an 8080 OUT port */
static void MachineOUT(uint8_t port, uint8_t value)
{
    switch (port) {
        case 2:
            /* Set shift amount (lower 3 bits only) */
            shift_amount = value & 0x07;
            break;
        case 4:
            /* Push new byte into the high end of the shift register */
            shift_reg = (shift_reg >> 8) | ((uint16_t)value << 8);
            break;
        /* Ports 3 and 5 are sound – not implemented yet */
        default: break;
    }
}

/* =============================================================
   libretro API implementation
   ============================================================= */

void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;

    /* Try to acquire the frontend log callback */
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;

    /* This core requires a ROM; disable no-game mode */
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
    cpu        = Init8080();
    cpu_halted = false;
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

    /* Reset all CPU and machine state */
    memset(cpu->memory, 0, 0x10000);
    cpu->pc         = 0;
    cpu->sp         = 0xF000;   /* safe default stack below VRAM */
    cpu->int_enable = 0;
    cpu_halted      = false;
    port1           = 0;
    port2           = 0;
    shift_reg       = 0;
    shift_amount    = 0;

    /* Copy ROM into the bottom of the 64 KB address space */
    if (game && game->data && game->size > 0) {
        size_t sz = (game->size > 0x10000) ? 0x10000 : game->size;
        memcpy(cpu->memory, game->data, sz);
    }

    /* Request XRGB8888 pixel format from the frontend */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    return true;
}

/* Map RetroPad buttons to the Space Invaders port 1 bit layout:
   bit 0 = coin   (Select)
   bit 2 = start  (Start)
   bit 4 = shoot  (A or B)
   bit 5 = left   (D-pad left)
   bit 6 = right  (D-pad right) */
static void UpdateInputs(void)
{
    if (!poll_cb || !input_cb) return;
    poll_cb();

    port1 = 0;

    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
        port1 |= (1 << 0);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
        port1 |= (1 << 2);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) ||
        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
        port1 |= (1 << 4);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
        port1 |= (1 << 5);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
        port1 |= (1 << 6);
}

/* Execute approximately `target` CPU cycles.
   IN (0xDB) and OUT (0xD3) opcodes are intercepted here before
   Emulate() is called so machine I/O is handled without touching
   i8080.c.

   CRASH GUARD: setjmp() catches any longjmp() fired by safe_exit()
   (which replaces exit() inside i8080.c at compile time).  If an
   unimplemented instruction is hit, we set cpu_halted and return
   cleanly instead of killing the process. */
static void RunCycles(int target)
{
    if (cpu_halted) return;   /* already faulted this session */

    /* Arm the longjmp guard – safe_exit() will land here */
    jmp_active = true;
    if (setjmp(cpu_exit_jmp) != 0) {
        /* Arrived here via longjmp from safe_exit().
           cpu_halted is already set; just clean up and return. */
        jmp_active = false;
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "[i8080] Unimplemented instruction at PC=0x%04X – "
                   "CPU halted. Reset the core to continue.\n",
                   cpu->pc);
        return;
    }

    int ran = 0;
    while (ran < target) {
        uint8_t op = cpu->memory[cpu->pc & 0xFFFF];

        if (op == 0xDB) {
            /* IN port – read from machine I/O instead of calling Emulate */
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            cpu->a  = MachineIN(port);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;   /* IN takes 10 cycles on the 8080 */

        } else if (op == 0xD3) {
            /* OUT port – write to machine I/O */
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            MachineOUT(port, cpu->a);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;   /* OUT takes 10 cycles on the 8080 */

        } else {
            /* Normal instruction – let the emulator core handle it.
               If the opcode is unimplemented, Emulate() calls
               UnimplementedInstruction() → exit() → safe_exit()
               → longjmp() back to setjmp() above. */
            ran += Emulate(cpu, false);
        }
    }

    jmp_active = false;   /* disarm the guard on normal exit */
}

/* Unpack the 1-bit VRAM (0x2400–0x3FFF) into the XRGB8888 framebuffer.
   Each byte encodes 8 horizontal pixels, LSB = leftmost pixel. */
static void RenderFrame(void)
{
    const uint8_t *vram = &cpu->memory[0x2400];

    for (int i = 0; i < (FB_W * FB_H / 8); i++) {
        uint8_t byte = vram[i];
        int     px   = i * 8;

        for (int bit = 0; bit < 8; bit++) {
            int x = (px + bit) % FB_W;
            int y = (px + bit) / FB_W;
            if (x < FB_W && y < FB_H) {
                frame_buf[y * FB_W + x] =
                    (byte & (1 << bit)) ? 0xFFFFFFFF   /* white pixel */
                                        : 0xFF000000;  /* black pixel */
            }
        }
    }
}

void retro_run(void)
{
    UpdateInputs();

    /* --- First half-frame ------------------------------------ */
    RunCycles(CYCLES_PER_HALF);

    /* Mid-screen interrupt: RST 1 (fires at roughly scan-line 96) */
    if (!cpu_halted && cpu->int_enable)
        GenInterrupt(cpu, 1);

    /* --- Second half-frame ----------------------------------- */
    RunCycles(CYCLES_PER_HALF);

    /* VBlank interrupt: RST 2 */
    if (!cpu_halted && cpu->int_enable)
        GenInterrupt(cpu, 2);

    /* Draw current VRAM contents to the framebuffer */
    RenderFrame();

    if (video_cb)
        video_cb(frame_buf, FB_W, FB_H, sizeof(frame_buf[0]) * FB_W);
}

void retro_unload_game(void)
{
    if (cpu) {
        memset(cpu->memory, 0, 0x10000);
        cpu->pc    = 0;
        cpu_halted = false;
    }
}

void retro_reset(void)
{
    if (cpu) {
        cpu->pc         = 0;
        cpu->sp         = 0xF000;
        cpu->int_enable = 0;
        cpu_halted      = false;   /* clear halt so execution resumes */
        port1           = 0;
        port2           = 0;
        shift_reg       = 0;
        shift_amount    = 0;
    }
}

/* Save state: serialise CPU registers + full 64 KB RAM.
   The memory pointer itself is replaced with NULL so we never
   store a host address in the save file. */
size_t retro_serialize_size(void)
{
    return sizeof(State8080) + 0x10000;
}

bool retro_serialize(void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    uint8_t  *p   = (uint8_t *)data;
    State8080  tmp = *cpu;
    tmp.memory = NULL;                    /* strip host pointer */
    memcpy(p, &tmp, sizeof(State8080));
    p += sizeof(State8080);
    memcpy(p, cpu->memory, 0x10000);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    const uint8_t *p     = (const uint8_t *)data;
    uint8_t       *saved = cpu->memory;   /* keep our allocation */
    memcpy(cpu, p, sizeof(State8080));
    cpu->memory = saved;                  /* restore host pointer */
    p += sizeof(State8080);
    memcpy(cpu->memory, p, 0x10000);
    cpu_halted = false;                   /* allow execution after restore */
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
