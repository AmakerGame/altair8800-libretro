/* =============================================================
   libretro.c – Intel 8080 / Altair 8800 RetroArch core
   =============================================================
   Full Altair 8800 emulation with:
     - Authentic front panel LED display (A0-A15, D0-D7, status LEDs)
     - Toggle switch input (address/data entry via RetroPad)
     - Serial terminal I/O (ports 0x00/0x01 – 88-SIO card)
     - Space Invaders machine I/O (shift register, ports 1-4)
     - Sound via audio_batch (port 3/5 beeper simulation)
     - EXAMINE / EXAMINE NEXT / DEPOSIT / DEPOSIT NEXT / RUN / STOP
     - Save states (registers + 64 KB RAM)
     - Crash protection: safe_exit() + setjmp/longjmp guard

   FRONT PANEL CONTROLS (RetroPad mapping)
   ─────────────────────────────────────────
   D-Pad Left / Right  – move cursor among the 16 address toggle switches
   D-Pad Up / Down     – move cursor among the  8 data  toggle switches
   B                   – flip the selected toggle switch (0 ↔ 1)
   A                   – DEPOSIT (write data switches → memory[address])
   X                   – EXAMINE (load memory[address] → data LEDs)
   Y                   – DEPOSIT NEXT  (addr++, then DEPOSIT)
   L1                  – EXAMINE NEXT  (addr++, then EXAMINE)
   R1                  – RESET  (PC = 0, SP = 0xF000)
   Start               – RUN / STOP toggle
   Select              – switch panel mode: ALTAIR ↔ SI (Space Invaders)

   TERMINAL (88-SIO serial card)
   ─────────────────────────────
   Port 0x00 – status: bit0=input ready, bit1=output ready (always 1)
   Port 0x01 – data:   read = next char from input ring buffer
                       write = char to terminal output ring buffer
   The frontend may read terminal output via RETRO_MEMORY_SAVE_RAM.
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
   Crash-protection: redirect exit() and printf() BEFORE
   i8080.h is included so every reference inside i8080.c
   resolves to our safe stubs.
   ---------------------------------------------------------- */
static jmp_buf cpu_exit_jmp;
static bool    cpu_halted = false;
static bool    jmp_active = false;

static void safe_exit(int code)
{
    cpu_halted = true;
    if (jmp_active)
        longjmp(cpu_exit_jmp, code ? code : 1);
}

static retro_log_printf_t log_cb = NULL;

static int safe_printf(const char *fmt, ...)
{
    if (!log_cb) return 0;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_cb(RETRO_LOG_INFO, "%s", buf);
    return n;
}

#define exit(c)      safe_exit(c)
#define printf(...)  safe_printf(__VA_ARGS__)

#include "i8080.h"

#undef exit
#undef printf

/* =============================================================
   Display / framebuffer
   ============================================================= */
#define FB_W   800
#define FB_H   480

static uint32_t frame_buf[FB_W * FB_H];
static State8080 *cpu = NULL;

/* =============================================================
   Timing  ~2 MHz @ 60 fps
   ============================================================= */
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
   Panel mode
   ============================================================= */
typedef enum { MODE_ALTAIR = 0, MODE_SI = 1 } PanelMode;
static PanelMode panel_mode = MODE_ALTAIR;

/* =============================================================
   Core options (parameters shown in RetroArch Quick Menu)
   ============================================================= */

/* Option keys */
#define OPT_CPU_SPEED   "altair8800_cpu_speed"
#define OPT_PANEL_MODE  "altair8800_panel_mode"
#define OPT_BEEPER      "altair8800_beeper"
#define OPT_AUTORUN     "altair8800_autorun"
#define OPT_STACK_ADDR  "altair8800_stack_addr"

/* Option current values */
static int  opt_cpu_speed   = 100;   /* % of 2 MHz (50/100/200/400) */
static bool opt_beeper_en   = true;
static bool opt_autorun     = false;
static int  opt_stack_addr  = 0xF000;

static void ApplyCoreOptions(retro_environment_t cb)
{
    struct retro_variable var;

    var.key = OPT_CPU_SPEED;
    var.value = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if      (!strcmp(var.value, "50"))  opt_cpu_speed = 50;
        else if (!strcmp(var.value, "100")) opt_cpu_speed = 100;
        else if (!strcmp(var.value, "200")) opt_cpu_speed = 200;
        else if (!strcmp(var.value, "400")) opt_cpu_speed = 400;
        else if (!strcmp(var.value, "800")) opt_cpu_speed = 800;
    }

    var.key = OPT_PANEL_MODE;
    var.value = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if (!strcmp(var.value, "space_invaders"))
            panel_mode = MODE_SI;
        else
            panel_mode = MODE_ALTAIR;
    }

    var.key = OPT_BEEPER;
    var.value = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        opt_beeper_en = (strcmp(var.value, "disabled") != 0);

    var.key = OPT_AUTORUN;
    var.value = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        opt_autorun = (strcmp(var.value, "enabled") == 0);

    var.key = OPT_STACK_ADDR;
    var.value = NULL;
    if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        if      (!strcmp(var.value, "0xF000")) opt_stack_addr = 0xF000;
        else if (!strcmp(var.value, "0xE000")) opt_stack_addr = 0xE000;
        else if (!strcmp(var.value, "0xD000")) opt_stack_addr = 0xD000;
        else if (!strcmp(var.value, "0xFF00")) opt_stack_addr = 0xFF00;
    }
}

static const struct retro_variable core_options[] = {
    {
        OPT_CPU_SPEED,
        "CPU Speed; 100|50|200|400|800"
    },
    {
        OPT_PANEL_MODE,
        "Panel Mode (requires reload); altair|space_invaders"
    },
    {
        OPT_BEEPER,
        "Beeper / Sound; enabled|disabled"
    },
    {
        OPT_AUTORUN,
        "Auto-Run on Load; disabled|enabled"
    },
    {
        OPT_STACK_ADDR,
        "Default Stack Pointer; 0xF000|0xE000|0xD000|0xFF00"
    },
    { NULL, NULL }   /* sentinel */
};

/* =============================================================
   Altair 8800 front-panel state
   ============================================================= */

/* 16 address toggle switches (A0–A15), 8 data switches (D0–D7) */
static uint16_t sw_addr   = 0x0000;   /* current address switch state */
static uint8_t  sw_data   = 0x00;     /* current data switch state    */

/* Which switch is highlighted by the cursor */
static int  cursor_addr   = 0;        /* 0-15, LSB first              */
static int  cursor_data   = 0;        /* 0-7,  LSB first              */
static bool cursor_on_addr = true;    /* true = addr row selected     */

/* RUN / STOP state */
static bool panel_running = false;

/* EXAMINE address latch (shown on address LEDs when stopped) */
static uint16_t led_addr  = 0x0000;
static uint8_t  led_data  = 0x00;

/* Status LEDs */
typedef struct {
    bool memr, inp, m1, out, hlta, stack, wO, inta;
    bool wait, hlda, inte, prot, run;
} StatusLEDs;
static StatusLEDs status_leds;

/* Debounce: track previous button state */
static uint32_t prev_buttons = 0;

/* =============================================================
   Space Invaders I/O (active when panel_mode == MODE_SI)
   ============================================================= */
static uint8_t  si_port1     = 0x00;
static uint8_t  si_port2     = 0x00;
static uint16_t shift_reg    = 0;
static uint8_t  shift_amount = 0;

/* =============================================================
   Serial terminal – 88-SIO ring buffers
   ============================================================= */
#define TERM_BUF 4096

static uint8_t  term_in_buf[TERM_BUF];    /* host → CPU  */
static int      term_in_head  = 0;
static int      term_in_tail  = 0;

static uint8_t  term_out_buf[TERM_BUF];   /* CPU → host  */
static int      term_out_head = 0;
static int      term_out_tail = 0;

static inline bool term_in_empty(void)  { return term_in_head  == term_in_tail;  }
static inline bool term_out_full(void)  { return ((term_out_tail + 1) % TERM_BUF) == term_out_head; }

static void term_push_in(uint8_t c)
{
    int next = (term_in_tail + 1) % TERM_BUF;
    if (next != term_in_head) {
        term_in_buf[term_in_tail] = c;
        term_in_tail = next;
    }
}

static uint8_t term_pop_in(void)
{
    if (term_in_empty()) return 0;
    uint8_t c = term_in_buf[term_in_head];
    term_in_head = (term_in_head + 1) % TERM_BUF;
    return c;
}

static void term_push_out(uint8_t c)
{
    if (!term_out_full()) {
        term_out_buf[term_out_tail] = c;
        term_out_tail = (term_out_tail + 1) % TERM_BUF;
    }
}

/* =============================================================
   Sound – simple beeper state
   ============================================================= */
#define SAMPLE_RATE      44100
#define SAMPLES_PER_FRAME (SAMPLE_RATE / 60)

static bool  beeper_on    = false;
static float beeper_phase = 0.0f;
#define BEEPER_FREQ 880.0f   /* Hz */

static void GenerateAudio(void)
{
    if (!audio_batch_cb) return;

    int16_t buf[SAMPLES_PER_FRAME * 2];
    float step = 2.0f * 3.14159265f * BEEPER_FREQ / (float)SAMPLE_RATE;

    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
        int16_t sample = 0;
        if (beeper_on && opt_beeper_en) {
            sample = (beeper_phase < 3.14159265f) ? 8000 : -8000;
            beeper_phase += step;
            if (beeper_phase >= 2.0f * 3.14159265f)
                beeper_phase -= 2.0f * 3.14159265f;
        }
        buf[i * 2]     = sample;
        buf[i * 2 + 1] = sample;
    }
    audio_batch_cb(buf, SAMPLES_PER_FRAME);
}

/* =============================================================
   Machine I/O – unified IN/OUT handler
   ============================================================= */
static uint8_t MachineIN(uint8_t port)
{
    if (panel_mode == MODE_SI) {
        /* Space Invaders hardware ports */
        switch (port) {
            case 0: return 0x0e;
            case 1: return si_port1;
            case 2: return si_port2;
            case 3: return (uint8_t)(shift_reg >> (8 - shift_amount));
            default: return 0xFF;
        }
    } else {
        /* Altair 8800 / 88-SIO serial card */
        switch (port) {
            /* Status: bit0=RX ready, bit1=TX ready (always 1) */
            case 0x00:
                return (uint8_t)((!term_in_empty() ? 0x01 : 0x00) | 0x02);
            /* Data read */
            case 0x01:
                return term_pop_in();
            default: return 0xFF;
        }
    }
}

static void MachineOUT(uint8_t port, uint8_t value)
{
    if (panel_mode == MODE_SI) {
        /* Space Invaders hardware ports */
        switch (port) {
            case 2: shift_amount = value & 0x07; break;
            case 3: beeper_on = (value & 0x01) != 0; break;
            case 4: shift_reg = (shift_reg >> 8) | ((uint16_t)value << 8); break;
            case 5: beeper_on = (value != 0); break;
            default: break;
        }
    } else {
        /* Altair 8800 / 88-SIO */
        switch (port) {
            case 0x00: break;                       /* control register – ignore */
            case 0x01: term_push_out(value); break; /* data write */
            default: break;
        }
    }
}

/* =============================================================
   Run CPU cycles with crash guard
   ============================================================= */
static void RunCycles(int target)
{
    if (cpu_halted || !panel_running) return;
    /* Scale cycles by CPU speed option */
    target = (target * opt_cpu_speed) / 100;

    jmp_active = true;
    if (setjmp(cpu_exit_jmp) != 0) {
        jmp_active = false;
        if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "[i8080] Unimplemented instruction at PC=0x%04X – "
                   "CPU halted. Reset to continue.\n",
                   cpu->pc);
        panel_running = false;
        status_leds.hlta = true;
        return;
    }

    int ran = 0;
    while (ran < target) {
        uint8_t op = cpu->memory[cpu->pc & 0xFFFF];

        /* Update status LEDs from opcode type */
        status_leds.memr  = true;
        status_leds.m1    = true;
        status_leds.run   = true;
        status_leds.wait  = false;
        status_leds.hlta  = false;
        status_leds.out   = (op == 0xD3);
        status_leds.inp   = (op == 0xDB);

        if (op == 0xDB) {
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            cpu->a  = MachineIN(port);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;

        } else if (op == 0xD3) {
            uint8_t port = cpu->memory[(cpu->pc + 1) & 0xFFFF];
            MachineOUT(port, cpu->a);
            cpu->pc = (cpu->pc + 2) & 0xFFFF;
            ran    += 10;

        } else {
            ran += Emulate(cpu, false);
        }

        /* Sync panel address/data LEDs to PC and memory under PC */
        led_addr = cpu->pc;
        led_data = cpu->memory[cpu->pc & 0xFFFF];
    }

    jmp_active = false;
}

/* =============================================================
   Input handling
   ============================================================= */

/* Helper to detect a fresh button press (not held) */
static inline bool just_pressed(uint32_t cur, uint32_t prev, unsigned btn)
{
    return (cur & (1u << btn)) && !(prev & (1u << btn));
}

static void UpdateAltairInputs(void)
{
    if (!poll_cb || !input_cb) return;
    poll_cb();

    uint32_t cur = 0;
    /* Read all RetroPad buttons into a bitmask */
    static const unsigned ids[] = {
        RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_UP,
        RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT,
        RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_A,
        RETRO_DEVICE_ID_JOYPAD_X,
        RETRO_DEVICE_ID_JOYPAD_L,
        RETRO_DEVICE_ID_JOYPAD_R,
    };
    for (unsigned i = 0; i < sizeof(ids)/sizeof(ids[0]); i++)
        if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, ids[i]))
            cur |= (1u << i);

    /* Map bitmask indices to button IDs */
#define IDX_B      0
#define IDX_Y      1
#define IDX_SELECT 2
#define IDX_START  3
#define IDX_UP     4
#define IDX_DOWN   5
#define IDX_LEFT   6
#define IDX_RIGHT  7
#define IDX_A      8
#define IDX_X      9
#define IDX_L      10
#define IDX_R      11

    /* SELECT: toggle panel mode */
    if (just_pressed(cur, prev_buttons, IDX_SELECT)) {
        panel_mode = (panel_mode == MODE_ALTAIR) ? MODE_SI : MODE_ALTAIR;
    }

    /* START: RUN / STOP */
    if (just_pressed(cur, prev_buttons, IDX_START)) {
        panel_running = !panel_running;
        cpu_halted    = false;
        status_leds.run  = panel_running;
        status_leds.wait = !panel_running;
    }

    if (!panel_running) {
        /* --- Panel control (STOP mode only) --- */

        /* Move cursor LEFT/RIGHT among address switches */
        if (just_pressed(cur, prev_buttons, IDX_LEFT)) {
            cursor_on_addr = true;
            cursor_addr = (cursor_addr + 1) % 16;
        }
        if (just_pressed(cur, prev_buttons, IDX_RIGHT)) {
            cursor_on_addr = true;
            cursor_addr = (cursor_addr + 15) % 16;
        }
        /* Move cursor UP/DOWN among data switches */
        if (just_pressed(cur, prev_buttons, IDX_UP)) {
            cursor_on_addr = false;
            cursor_data = (cursor_data + 1) % 8;
        }
        if (just_pressed(cur, prev_buttons, IDX_DOWN)) {
            cursor_on_addr = false;
            cursor_data = (cursor_data + 7) % 8;
        }

        /* B: flip selected switch */
        if (just_pressed(cur, prev_buttons, IDX_B)) {
            if (cursor_on_addr)
                sw_addr ^= (1u << cursor_addr);
            else
                sw_data ^= (1u << cursor_data);
        }

        /* X: EXAMINE – show memory[sw_addr] on data LEDs */
        if (just_pressed(cur, prev_buttons, IDX_X)) {
            led_addr = sw_addr;
            led_data = cpu->memory[sw_addr];
            status_leds.memr = true;
            status_leds.wait = true;
        }

        /* L1: EXAMINE NEXT */
        if (just_pressed(cur, prev_buttons, IDX_L)) {
            sw_addr  = (sw_addr + 1) & 0xFFFF;
            led_addr = sw_addr;
            led_data = cpu->memory[sw_addr];
            status_leds.memr = true;
            status_leds.wait = true;
        }

        /* A: DEPOSIT – write sw_data → memory[sw_addr] */
        if (just_pressed(cur, prev_buttons, IDX_A)) {
            cpu->memory[sw_addr] = sw_data;
            led_addr = sw_addr;
            led_data = sw_data;
            status_leds.wO   = true;
            status_leds.wait = true;
        }

        /* Y: DEPOSIT NEXT */
        if (just_pressed(cur, prev_buttons, IDX_Y)) {
            sw_addr = (sw_addr + 1) & 0xFFFF;
            cpu->memory[sw_addr] = sw_data;
            led_addr = sw_addr;
            led_data = sw_data;
            status_leds.wO   = true;
            status_leds.wait = true;
        }

        /* R1: RESET */
        if (just_pressed(cur, prev_buttons, IDX_R)) {
            cpu->pc         = sw_addr;   /* jump to address shown on switches */
            cpu->sp         = (uint16_t)opt_stack_addr;
            cpu->int_enable = 0;
            cpu_halted      = false;
            led_addr = cpu->pc;
            led_data = cpu->memory[cpu->pc];
            status_leds.run  = false;
            status_leds.wait = true;
        }
    }

    prev_buttons = cur;
}

static void UpdateSIInputs(void)
{
    if (!poll_cb || !input_cb) return;
    poll_cb();

    si_port1 = 0;
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
        si_port1 |= (1 << 0);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
        si_port1 |= (1 << 2);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) ||
        input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
        si_port1 |= (1 << 4);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
        si_port1 |= (1 << 5);
    if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
        si_port1 |= (1 << 6);
}

/* =============================================================
   Rendering – authentic Altair 8800 front panel
   ============================================================= */

/* Colour palette */
#define COL_BG          0xFF1A1A1A   /* dark chassis            */
#define COL_PANEL       0xFF2B2B2B   /* panel face              */
#define COL_CHROME      0xFF8899AA   /* chrome accents          */
#define COL_LED_ON      0xFFFF4400   /* lit LED (orange-red)    */
#define COL_LED_OFF     0xFF3A1800   /* unlit LED               */
#define COL_SW_UP       0xFFCCCCCC   /* switch lever up         */
#define COL_SW_DOWN     0xFF888888   /* switch lever down       */
#define COL_SW_CURSOR   0xFF00FF88   /* highlighted switch      */
#define COL_TEXT        0xFFDDDDDD
#define COL_TEXT_DIM    0xFF888888
#define COL_STATUS_ON   0xFFFFAA00
#define COL_STATUS_OFF  0xFF332200
#define COL_TERM_BG     0xFF001800   /* terminal area bg        */
#define COL_TERM_TEXT   0xFF00FF44   /* terminal text (green)   */
#define COL_RUN_LED     0xFF00FF00
#define COL_WAIT_LED    0xFFFFFF00

static void DrawRect(int x, int y, int w, int h, uint32_t col)
{
    for (int row = y; row < y + h && row < FB_H; row++)
        for (int col2 = x; col2 < x + w && col2 < FB_W; col2++)
            if (row >= 0 && col2 >= 0)
                frame_buf[row * FB_W + col2] = col;
}

/* Draw a small circle/LED at (cx,cy) with radius r */
static void DrawLED(int cx, int cy, int r, uint32_t col)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                    frame_buf[py * FB_W + px] = col;
            }
}

/* Very small 3×5 bitmap font – only hex digits + a few letters */
static const uint8_t font3x5[128][5] = {
    ['0'] = {0x07,0x05,0x05,0x05,0x07},
    ['1'] = {0x02,0x06,0x02,0x02,0x07},
    ['2'] = {0x07,0x01,0x07,0x04,0x07},
    ['3'] = {0x07,0x01,0x07,0x01,0x07},
    ['4'] = {0x05,0x05,0x07,0x01,0x01},
    ['5'] = {0x07,0x04,0x07,0x01,0x07},
    ['6'] = {0x07,0x04,0x07,0x05,0x07},
    ['7'] = {0x07,0x01,0x01,0x01,0x01},
    ['8'] = {0x07,0x05,0x07,0x05,0x07},
    ['9'] = {0x07,0x05,0x07,0x01,0x07},
    ['A'] = {0x07,0x05,0x07,0x05,0x05},
    ['B'] = {0x06,0x05,0x06,0x05,0x06},
    ['C'] = {0x07,0x04,0x04,0x04,0x07},
    ['D'] = {0x06,0x05,0x05,0x05,0x06},
    ['E'] = {0x07,0x04,0x07,0x04,0x07},
    ['F'] = {0x07,0x04,0x07,0x04,0x04},
    ['H'] = {0x05,0x05,0x07,0x05,0x05},
    ['I'] = {0x07,0x02,0x02,0x02,0x07},
    ['L'] = {0x04,0x04,0x04,0x04,0x07},
    ['M'] = {0x05,0x07,0x07,0x05,0x05},
    ['N'] = {0x05,0x07,0x07,0x07,0x05},
    ['O'] = {0x07,0x05,0x05,0x05,0x07},
    ['P'] = {0x07,0x05,0x07,0x04,0x04},
    ['R'] = {0x07,0x05,0x07,0x06,0x05},
    ['S'] = {0x07,0x04,0x07,0x01,0x07},
    ['T'] = {0x07,0x02,0x02,0x02,0x02},
    ['U'] = {0x05,0x05,0x05,0x05,0x07},
    ['W'] = {0x05,0x05,0x07,0x07,0x05},
    ['X'] = {0x05,0x05,0x02,0x05,0x05},
    [' '] = {0x00,0x00,0x00,0x00,0x00},
    [':'] = {0x00,0x02,0x00,0x02,0x00},
    ['-'] = {0x00,0x00,0x07,0x00,0x00},
    ['>'] = {0x04,0x02,0x01,0x02,0x04},
};

static void DrawChar(int x, int y, char c, uint32_t col, int scale)
{
    if ((unsigned char)c >= 128) return;
    const uint8_t *glyph = font3x5[(unsigned char)c];
    for (int row = 0; row < 5; row++) {
        for (int col2 = 0; col2 < 3; col2++) {
            if (glyph[row] & (4 >> col2)) {
                DrawRect(x + col2*scale, y + row*scale, scale, scale, col);
            }
        }
    }
}

static void DrawStr(int x, int y, const char *s, uint32_t col, int scale)
{
    while (*s) {
        DrawChar(x, y, *s++, col, scale);
        x += 4 * scale;
    }
}

/* Draw a hex word as 4 hex digits */
static void DrawHex16(int x, int y, uint16_t val, uint32_t col, int scale)
{
    char buf[5];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 12) & 0xF];
    buf[1] = hex[(val >>  8) & 0xF];
    buf[2] = hex[(val >>  4) & 0xF];
    buf[3] = hex[(val >>  0) & 0xF];
    buf[4] = '\0';
    DrawStr(x, y, buf, col, scale);
}

static void DrawHex8(int x, int y, uint8_t val, uint32_t col, int scale)
{
    char buf[3];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = hex[(val >> 4) & 0xF];
    buf[1] = hex[(val >> 0) & 0xF];
    buf[2] = '\0';
    DrawStr(x, y, buf, col, scale);
}

/* -------------------------------------------------------
   Terminal display area (bottom portion of screen)
   ------------------------------------------------------- */
#define TERM_COLS     64
#define TERM_ROWS     8
#define TERM_CHAR_W   6
#define TERM_CHAR_H   8

static char term_screen[TERM_ROWS][TERM_COLS];
static int  term_cur_col = 0;
static int  term_cur_row = 0;

static void term_scroll(void)
{
    memmove(term_screen[0], term_screen[1], (TERM_ROWS - 1) * TERM_COLS);
    memset(term_screen[TERM_ROWS - 1], ' ', TERM_COLS);
    term_cur_row = TERM_ROWS - 1;
}

static void term_put_char(uint8_t c)
{
    if (c == '\n' || c == '\r') {
        term_cur_col = 0;
        term_cur_row++;
        if (term_cur_row >= TERM_ROWS)
            term_scroll();
        return;
    }
    if (c == '\b') {
        if (term_cur_col > 0) term_cur_col--;
        term_screen[term_cur_row][term_cur_col] = ' ';
        return;
    }
    if (c >= 0x20 && c < 0x7F) {
        term_screen[term_cur_row][term_cur_col++] = (char)c;
        if (term_cur_col >= TERM_COLS) {
            term_cur_col = 0;
            term_cur_row++;
            if (term_cur_row >= TERM_ROWS)
                term_scroll();
        }
    }
}

static void DrainTermOutput(void)
{
    while (term_out_head != term_out_tail) {
        uint8_t c = term_out_buf[term_out_head];
        term_out_head = (term_out_head + 1) % TERM_BUF;
        term_put_char(c);
    }
}

static void RenderTerminal(int base_y)
{
    /* Background */
    DrawRect(0, base_y, FB_W, FB_H - base_y, COL_TERM_BG);
    DrawStr(4, base_y + 2, "TERMINAL  88-SIO", COL_TERM_TEXT, 1);

    int ty = base_y + 12;
    for (int row = 0; row < TERM_ROWS; row++) {
        int tx = 4;
        for (int col2 = 0; col2 < TERM_COLS; col2++) {
            char c = term_screen[row][col2];
            DrawChar(tx, ty, c ? c : ' ', COL_TERM_TEXT, 1);
            tx += 5;
        }
        ty += TERM_CHAR_H;
    }
    /* Cursor blink */
    static int blink = 0;
    if ((blink++ / 30) & 1) {
        DrawRect(4 + term_cur_col * 5, base_y + 12 + term_cur_row * TERM_CHAR_H,
                 4, 6, COL_TERM_TEXT);
    }
}

/* -------------------------------------------------------
   Main panel render
   ------------------------------------------------------- */
static void RenderAltairPanel(void)
{
    /* Clear */
    DrawRect(0, 0, FB_W, FB_H, COL_BG);

    /* Panel face */
    DrawRect(10, 10, FB_W - 20, 280, COL_PANEL);

    /* === Title === */
    DrawStr(14, 14, "ALTAIR  8800", COL_TEXT, 2);

    /* === RUN / WAIT / HLTA / STATUS indicators === */
    int sx = 340, sy = 14;
    DrawStr(sx, sy, "RUN",  COL_TEXT_DIM, 1);
    DrawLED(sx + 20, sy + 3, 4, panel_running ? COL_RUN_LED : 0xFF333300);
    DrawStr(sx + 30, sy, "WAIT", COL_TEXT_DIM, 1);
    DrawLED(sx + 58, sy + 3, 4, status_leds.wait ? COL_WAIT_LED : 0xFF333300);
    DrawStr(sx + 70, sy, "HLTA", COL_TEXT_DIM, 1);
    DrawLED(sx + 98, sy + 3, 4, status_leds.hlta ? COL_LED_ON : COL_LED_OFF);
    DrawStr(sx + 110, sy, "INTE", COL_TEXT_DIM, 1);
    DrawLED(sx + 138, sy + 3, 4, cpu->int_enable ? COL_STATUS_ON : COL_STATUS_OFF);

    /* === Address LEDs (A15–A0) === */
    int led_y = 40;
    DrawStr(14, led_y, "ADDRESS", COL_TEXT_DIM, 1);
    DrawStr(14, led_y + 10, "A15                          A0", COL_TEXT_DIM, 1);

    int led_x0 = 14;
    int led_spacing = 47;
    for (int i = 15; i >= 0; i--) {
        int xi = led_x0 + (15 - i) * led_spacing / 4;  /* distribute 16 LEDs */
        /* use simpler even spacing */
        xi = led_x0 + (15 - i) * 47;
        if (xi > FB_W - 20) xi = FB_W - 20;
        bool on = ((led_addr >> i) & 1) != 0;
        DrawLED(xi + 3, led_y + 26, 5, on ? COL_LED_ON : COL_LED_OFF);
    }

    /* Hex value */
    DrawStr(FB_W - 80, led_y + 20, "ADDR:", COL_TEXT_DIM, 1);
    DrawHex16(FB_W - 48, led_y + 20, led_addr, COL_TEXT, 2);

    /* === Data LEDs (D7–D0) === */
    int dled_y = led_y + 50;
    DrawStr(14, dled_y, "DATA", COL_TEXT_DIM, 1);
    DrawStr(14, dled_y + 10, "D7                D0", COL_TEXT_DIM, 1);

    for (int i = 7; i >= 0; i--) {
        int xi = led_x0 + (7 - i) * 47;
        if (xi > FB_W - 20) xi = FB_W - 20;
        bool on = ((led_data >> i) & 1) != 0;
        DrawLED(xi + 3, dled_y + 26, 5, on ? COL_LED_ON : COL_LED_OFF);
    }

    DrawStr(FB_W - 80, dled_y + 20, "DATA:", COL_TEXT_DIM, 1);
    DrawHex8(FB_W - 32, dled_y + 20, led_data, COL_TEXT, 2);

    /* === Status LEDs row === */
    int stled_y = dled_y + 50;
    const char *stnames[] = {"MEMR","INP","M1","OUT","HLTA","STACK","WO","INTA"};
    bool stvals[8] = {
        (bool)status_leds.memr, (bool)status_leds.inp, (bool)status_leds.m1,
        (bool)status_leds.out,  (bool)status_leds.hlta,(bool)status_leds.stack,
        (bool)status_leds.wO,   (bool)status_leds.inta
    };
    for (int i = 0; i < 8; i++) {
        int xi = 14 + i * 94;
        DrawStr(xi, stled_y, stnames[i], COL_TEXT_DIM, 1);
        DrawLED(xi + 10, stled_y + 14, 5, stvals[i] ? COL_STATUS_ON : COL_STATUS_OFF);
    }

    /* === Address toggle switches === */
    int sw_y = stled_y + 40;
    DrawStr(14, sw_y - 10, "ADDRESS SWITCHES  (Left/Right to move,  B to flip)", COL_TEXT_DIM, 1);
    for (int i = 15; i >= 0; i--) {
        int xi = led_x0 + (15 - i) * 47;
        if (xi > FB_W - 20) xi = FB_W - 20;
        bool bit_on = ((sw_addr >> i) & 1) != 0;
        bool is_cursor = cursor_on_addr && (cursor_addr == i);

        uint32_t sw_col = is_cursor ? COL_SW_CURSOR : (bit_on ? COL_SW_UP : COL_SW_DOWN);

        /* Switch body */
        DrawRect(xi, sw_y, 10, 18, 0xFF444444);
        /* Lever */
        if (bit_on)
            DrawRect(xi + 2, sw_y + 1, 6, 8, sw_col);
        else
            DrawRect(xi + 2, sw_y + 9, 6, 8, sw_col);
    }

    /* === Data toggle switches === */
    int dsw_y = sw_y + 30;
    DrawStr(14, dsw_y - 10, "DATA SWITCHES  (Up/Down to move,  B to flip)", COL_TEXT_DIM, 1);
    for (int i = 7; i >= 0; i--) {
        int xi = led_x0 + (7 - i) * 47;
        if (xi > FB_W - 20) xi = FB_W - 20;
        bool bit_on = ((sw_data >> i) & 1) != 0;
        bool is_cursor = !cursor_on_addr && (cursor_data == i);

        uint32_t sw_col = is_cursor ? COL_SW_CURSOR : (bit_on ? COL_SW_UP : COL_SW_DOWN);

        DrawRect(xi, dsw_y, 10, 18, 0xFF444444);
        if (bit_on)
            DrawRect(xi + 2, dsw_y + 1, 6, 8, sw_col);
        else
            DrawRect(xi + 2, dsw_y + 9, 6, 8, sw_col);
    }

    DrawStr(FB_W - 80, dsw_y + 4, "SW:", COL_TEXT_DIM, 1);
    DrawHex16(FB_W - 64, dsw_y + 4,
              (uint16_t)((sw_addr & 0xFF00) | (sw_data & 0xFF)),
              COL_TEXT, 1);

    /* === CPU register readout === */
    int reg_y = dsw_y + 32;
    DrawStr(14, reg_y, "PC:", COL_TEXT_DIM, 1);
    DrawHex16(38, reg_y, cpu->pc, COL_TEXT, 1);
    DrawStr(90, reg_y, "SP:", COL_TEXT_DIM, 1);
    DrawHex16(114, reg_y, cpu->sp, COL_TEXT, 1);
    DrawStr(170, reg_y, "A:", COL_TEXT_DIM, 1);
    DrawHex8(186, reg_y, cpu->a, COL_TEXT, 1);
    DrawStr(210, reg_y, "B:", COL_TEXT_DIM, 1);
    DrawHex8(226, reg_y, cpu->b, COL_TEXT, 1);
    DrawStr(250, reg_y, "C:", COL_TEXT_DIM, 1);
    DrawHex8(266, reg_y, cpu->c, COL_TEXT, 1);
    DrawStr(290, reg_y, "D:", COL_TEXT_DIM, 1);
    DrawHex8(306, reg_y, cpu->d, COL_TEXT, 1);
    DrawStr(330, reg_y, "E:", COL_TEXT_DIM, 1);
    DrawHex8(346, reg_y, cpu->e, COL_TEXT, 1);
    DrawStr(370, reg_y, "H:", COL_TEXT_DIM, 1);
    DrawHex8(386, reg_y, cpu->h, COL_TEXT, 1);
    DrawStr(410, reg_y, "L:", COL_TEXT_DIM, 1);
    DrawHex8(426, reg_y, cpu->l, COL_TEXT, 1);

    /* Flags */
    DrawStr(470, reg_y, "Z:", COL_TEXT_DIM, 1);
    DrawChar(486, reg_y, cpu->cc.z ? '1' : '0', COL_TEXT, 1);
    DrawStr(496, reg_y, "S:", COL_TEXT_DIM, 1);
    DrawChar(512, reg_y, cpu->cc.s ? '1' : '0', COL_TEXT, 1);
    DrawStr(522, reg_y, "P:", COL_TEXT_DIM, 1);
    DrawChar(538, reg_y, cpu->cc.p ? '1' : '0', COL_TEXT, 1);
    DrawStr(548, reg_y, "CY:", COL_TEXT_DIM, 1);
    DrawChar(572, reg_y, cpu->cc.cy ? '1' : '0', COL_TEXT, 1);

    /* Panel mode label */
    DrawStr(620, reg_y, panel_mode == MODE_ALTAIR ? "MODE:ALT" : "MODE:SI ", COL_TEXT_DIM, 1);

    /* === Button legend === */
    int leg_y = reg_y + 14;
    DrawStr(14, leg_y,
            "A=DEPOSIT  X=EXAMINE  Y=DEP.NXT  L=EX.NXT  R=RESET  START=RUN/STOP  SEL=MODE",
            COL_TEXT_DIM, 1);

    /* === Terminal === */
    RenderTerminal(300);
}

/* Simple Space Invaders VRAM renderer (original behaviour) */
static void RenderSI(void)
{
    const uint8_t *vram = &cpu->memory[0x2400];
    for (int i = 0; i < (256 * 256 / 8); i++) {
        uint8_t byte = vram[i];
        int px = i * 8;
        for (int bit = 0; bit < 8; bit++) {
            /* Map 256×256 to 800×480 – scale 3×1 */
            int sx = (px + bit) % 256;
            int sy = (px + bit) / 256;
            int dx = sx * 3;
            int dy = sy * 480 / 256;
            uint32_t col = (byte & (1 << bit)) ? 0xFFFFFFFF : 0xFF000000;
            if (dx + 3 <= FB_W && dy < FB_H) {
                frame_buf[dy * FB_W + dx]     = col;
                frame_buf[dy * FB_W + dx + 1] = col;
                frame_buf[dy * FB_W + dx + 2] = col;
            }
        }
    }
}

static void RenderFrame(void)
{
    memset(frame_buf, 0, sizeof(frame_buf));
    DrainTermOutput();

    if (panel_mode == MODE_SI)
        RenderSI();
    else
        RenderAltairPanel();
}

/* =============================================================
   libretro API
   ============================================================= */

void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;

    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;

    bool no_rom = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_rom);

    /* Register core options */
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)core_options);

    /* Apply defaults immediately */
    ApplyCoreOptions(cb);
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
    memset(term_screen, ' ', sizeof(term_screen));
    memset(&status_leds, 0, sizeof(status_leds));
    status_leds.wait = true;
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
    info->library_name     = "altair8800";
    info->library_version  = "v2.0.0";
    info->need_fullpath    = false;
    info->valid_extensions = "bin|hex|com|rom|img|8080|tap|cpm|emu|prg|out|ram";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = (double)SAMPLE_RATE;
    info->geometry.base_width   = FB_W;
    info->geometry.base_height  = FB_H;
    info->geometry.max_width    = FB_W;
    info->geometry.max_height   = FB_H;
    info->geometry.aspect_ratio = (float)FB_W / (float)FB_H;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!cpu) cpu = Init8080();

    memset(cpu->memory, 0, 0x10000);
    cpu->pc         = 0;
    cpu->sp         = 0xF000;
    cpu->int_enable = 0;
    cpu_halted      = false;
    panel_running   = false;

    sw_addr = 0; sw_data = 0;
    led_addr = 0; led_data = 0;
    cursor_addr = 0; cursor_data = 0; cursor_on_addr = true;
    prev_buttons = 0;
    shift_reg = 0; shift_amount = 0;
    si_port1 = 0; si_port2 = 0;
    beeper_on = false; beeper_phase = 0.0f;
    term_in_head = term_in_tail = 0;
    term_out_head = term_out_tail = 0;
    memset(term_screen, ' ', sizeof(term_screen));
    term_cur_col = 0; term_cur_row = 0;
    memset(&status_leds, 0, sizeof(status_leds));
    status_leds.wait = true;

    if (game && game->data && game->size > 0) {
        size_t sz = (game->size > 0x10000) ? 0x10000 : game->size;
        memcpy(cpu->memory, game->data, sz);
    }

    /* Apply current options */
    if (env_cb) ApplyCoreOptions(env_cb);

    /* Auto-run if option enabled */
    if (opt_autorun) {
        panel_running = true;
        status_leds.run  = true;
        status_leds.wait = false;
    }

    /* Apply stack pointer from option */
    cpu->sp = (uint16_t)opt_stack_addr;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    return true;
}

void retro_run(void)
{
    /* Check if any core option changed this frame */
    bool updated = false;
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        ApplyCoreOptions(env_cb);

    if (panel_mode == MODE_SI) {
        UpdateSIInputs();
    } else {
        UpdateAltairInputs();
    }

    /* First CPU half */
    RunCycles(CYCLES_PER_HALF);

    /* Mid-frame interrupt (RST 1) – used by Space Invaders */
    if (!cpu_halted && panel_running && cpu->int_enable)
        GenInterrupt(cpu, 1);

    /* Second CPU half */
    RunCycles(CYCLES_PER_HALF);

    /* VBlank interrupt (RST 2) */
    if (!cpu_halted && panel_running && cpu->int_enable)
        GenInterrupt(cpu, 2);

    RenderFrame();
    GenerateAudio();

    if (video_cb)
        video_cb(frame_buf, FB_W, FB_H, sizeof(frame_buf[0]) * FB_W);
}

void retro_unload_game(void)
{
    if (cpu) {
        memset(cpu->memory, 0, 0x10000);
        cpu->pc    = 0;
        cpu_halted = false;
        panel_running = false;
    }
}

void retro_reset(void)
{
    if (cpu) {
        cpu->pc         = 0;
        cpu->sp         = 0xF000;
        cpu->int_enable = 0;
        cpu_halted      = false;
        panel_running   = false;
        led_addr = 0; led_data = 0;
        si_port1 = 0; si_port2 = 0;
        shift_reg = 0; shift_amount = 0;
        beeper_on = false;
        memset(&status_leds, 0, sizeof(status_leds));
        status_leds.wait = true;
    }
}

/* =============================================================
   Save states
   ============================================================= */
typedef struct {
    State8080  regs;              /* CPU registers (memory ptr = NULL) */
    uint16_t   sw_addr;
    uint8_t    sw_data;
    uint16_t   led_addr;
    uint8_t    led_data;
    bool       panel_running;
    bool       cpu_halted;
    uint16_t   shift_reg;
    uint8_t    shift_amount;
    uint8_t    si_port1;
    uint8_t    si_port2;
    /* 64 KB RAM follows immediately */
} SaveBlock;

size_t retro_serialize_size(void)
{
    return sizeof(SaveBlock) + 0x10000;
}

bool retro_serialize(void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    SaveBlock *blk = (SaveBlock *)data;
    blk->regs          = *cpu;
    blk->regs.memory   = NULL;
    blk->sw_addr       = sw_addr;
    blk->sw_data       = sw_data;
    blk->led_addr      = led_addr;
    blk->led_data      = led_data;
    blk->panel_running = panel_running;
    blk->cpu_halted    = cpu_halted;
    blk->shift_reg     = shift_reg;
    blk->shift_amount  = shift_amount;
    blk->si_port1      = si_port1;
    blk->si_port2      = si_port2;

    memcpy((uint8_t *)data + sizeof(SaveBlock), cpu->memory, 0x10000);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (!cpu || size < retro_serialize_size()) return false;

    const SaveBlock *blk = (const SaveBlock *)data;
    uint8_t *mem = cpu->memory;

    *cpu           = blk->regs;
    cpu->memory    = mem;
    sw_addr        = blk->sw_addr;
    sw_data        = blk->sw_data;
    led_addr       = blk->led_addr;
    led_data       = blk->led_data;
    panel_running  = blk->panel_running;
    cpu_halted     = blk->cpu_halted;
    shift_reg      = blk->shift_reg;
    shift_amount   = blk->shift_amount;
    si_port1       = blk->si_port1;
    si_port2       = blk->si_port2;

    memcpy(cpu->memory, (const uint8_t *)data + sizeof(SaveBlock), 0x10000);
    return true;
}

/* =============================================================
   Expose terminal output as SAVE_RAM so frontends can read it
   ============================================================= */
void *retro_get_memory_data(unsigned id)
{
    if (!cpu) return NULL;
    if (id == RETRO_MEMORY_SYSTEM_RAM) return cpu->memory;
    if (id == RETRO_MEMORY_SAVE_RAM)   return term_out_buf;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM) return 0x10000;
    if (id == RETRO_MEMORY_SAVE_RAM)   return TERM_BUF;
    return 0;
}

/* =============================================================
   Stubs required by the libretro ABI
   ============================================================= */
void retro_set_controller_port_device(unsigned port, unsigned device)
{ (void)port; (void)device; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }

bool retro_load_game_special(unsigned type,
                              const struct retro_game_info *info,
                              size_t num)
{ (void)type; (void)info; (void)num; return false; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
