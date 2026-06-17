/*
 * smolnes - NES emulator ported to ELKS direct VGA 640x480 16-color mode
 *
 * Original SDL2 version by: (smolnes authors)
 * ELKS/VGA port: proof of concept
 *
 * VGA mode 0x12: 640x480, 16 colors, planar (EGA-style)
 *   Video memory: 0xA0000 (far pointer on 16-bit, mmap on ELKS 32-bit)
 *   Sequencer, GC, CRTC accessed via I/O ports.
 *
 * NES output: 256x224 pixels
 * We draw it centered: x_off = (640 - 256*2) / 2 = 64, y_off = (480-224)/2 = 128
 * Each NES pixel is drawn as a 2x1 block (2 wide, 1 tall) for 512x224 display.
 * (Stretching to 2x2 would give 512x448, also fits fine if preferred.)
 *
 * EGA 16-color palette (indices 0-15):
 *  0=Black, 1=Blue, 2=Green, 3=Cyan, 4=Red, 5=Magenta, 6=Brown/Dark Yellow,
 *  7=Light Gray, 8=Dark Gray, 9=Bright Blue, 10=Bright Green, 11=Bright Cyan,
 *  12=Bright Red, 13=Bright Magenta, 14=Yellow, 15=White
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * VGA I/O port definitions
 * ------------------------------------------------------------------------- */
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_MISC_WRITE   0x3C2
#define VGA_AC_INDEX     0x3C0
#define VGA_AC_READ      0x3C1
#define VGA_INPUT_STATUS 0x3DA

/* VGA video memory base for mode 0x12 */
#define VGA_MEM_BASE 0xA0000UL

/* Screen geometry */
#define SCREEN_W   640
#define SCREEN_H   480
#define SCREEN_STRIDE (SCREEN_W / 8)  /* bytes per row in VGA planar */

/* NES output geometry */
#define NES_W      256
#define NES_H      224
/* Center the 2x-wide NES image */
#define NES_XOFF   ((SCREEN_W - NES_W * 2) / 2)   /* = 64 */
#define NES_YOFF   ((SCREEN_H - NES_H) / 2)        /* = 128 */

/* -------------------------------------------------------------------------
 * Port I/O - ELKS provides inb/outb via <sys/io.h> or inline asm.
 * We use inline asm for portability across ELKS versions.
 * ------------------------------------------------------------------------- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* -------------------------------------------------------------------------
 * VGA memory access
 * On ELKS (real-mode or small model), video RAM is at 0xA000:0000.
 * We use a far pointer via __far or segment tricks.
 * For a flat 32-bit ELKS build, just cast to pointer.
 * ------------------------------------------------------------------------- */

/* Write a byte to VGA planar memory at byte offset `offset` */
static void vga_poke(uint32_t offset, uint8_t val) {
    volatile uint8_t *vram = (volatile uint8_t *)(VGA_MEM_BASE + offset);
    *vram = val;
}

/* Read a byte from VGA planar memory */
static uint8_t vga_peek(uint32_t offset) {
    volatile uint8_t *vram = (volatile uint8_t *)(VGA_MEM_BASE + offset);
    return *vram;
}

/* -------------------------------------------------------------------------
 * VGA mode 0x12 setup (640x480 16 color)
 * Tables from FreeVGA / RBIL.
 * ------------------------------------------------------------------------- */

static uint8_t misc_output = 0xE3;

static uint8_t seq_regs[5] = {
    0x00, 0x01, 0x0F, 0x00, 0x06
};

static uint8_t crtc_regs[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80,
    0x0B, 0x3E, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7,
    0x04, 0xE3, 0xFF
};

static uint8_t gc_regs[9] = {
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x05, 0x0F, 0xFF
};

/* Default EGA/VGA 16-color DAC palette (6-bit R,G,B each, 0-63) */
static uint8_t ega_dac[16][3] = {
    { 0,  0,  0},  /* 0  Black        */
    { 0,  0, 42},  /* 1  Blue         */
    { 0, 42,  0},  /* 2  Green        */
    { 0, 42, 42},  /* 3  Cyan         */
    {42,  0,  0},  /* 4  Red          */
    {42,  0, 42},  /* 5  Magenta      */
    {42, 21,  0},  /* 6  Brown        */
    {42, 42, 42},  /* 7  Light Gray   */
    {21, 21, 21},  /* 8  Dark Gray    */
    {21, 21, 63},  /* 9  Bright Blue  */
    {21, 63, 21},  /* 10 Bright Green */
    {21, 63, 63},  /* 11 Bright Cyan  */
    {63, 21, 21},  /* 12 Bright Red   */
    {63, 21, 63},  /* 13 Bright Magenta */
    {63, 63, 21},  /* 14 Yellow       */
    {63, 63, 63},  /* 15 White        */
};

static void vga_set_mode12(void) {
    int i;

    /* Misc output */
    outb(VGA_MISC_WRITE, misc_output);

    /* Sequencer */
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x03); /* reset */
    for (i = 1; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, seq_regs[i]);
    }
    outb(VGA_SEQ_INDEX, 0x00); outb(VGA_SEQ_DATA, 0x03); /* end reset */

    /* CRTC - unlock then write */
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, inb(VGA_CRTC_DATA) & 0x7F);
    for (i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, crtc_regs[i]);
    }

    /* Graphics controller */
    for (i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, gc_regs[i]);
    }

    /* Attribute controller - palette identity map (0->0 ... 15->15) */
    inb(VGA_INPUT_STATUS); /* reset flip-flop */
    for (i = 0; i < 16; i++) {
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_INDEX, i);
    }
    /* Attribute mode, overscan, color plane enable, horizontal pixel pan */
    outb(VGA_AC_INDEX, 0x10); outb(VGA_AC_INDEX, 0x01);
    outb(VGA_AC_INDEX, 0x11); outb(VGA_AC_INDEX, 0x00);
    outb(VGA_AC_INDEX, 0x12); outb(VGA_AC_INDEX, 0x0F);
    outb(VGA_AC_INDEX, 0x13); outb(VGA_AC_INDEX, 0x00);
    outb(VGA_AC_INDEX, 0x14); outb(VGA_AC_INDEX, 0x00);
    outb(VGA_AC_INDEX, 0x20); outb(VGA_AC_INDEX, 0x00); /* enable video */

    /* Clear screen (all planes, color 0) */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x0F); /* write all planes */
    outb(VGA_GC_INDEX,  0x05); outb(VGA_GC_DATA,  0x02); /* write mode 2 */
    outb(VGA_GC_INDEX,  0x00); outb(VGA_GC_DATA,  0x00); /* set/reset = 0 */
    outb(VGA_GC_INDEX,  0x01); outb(VGA_GC_DATA,  0x0F); /* enable set/reset */
    {
        uint32_t off;
        for (off = 0; off < (uint32_t)(SCREEN_STRIDE * SCREEN_H); off++)
            vga_poke(off, 0x00);
    }
    /* Restore GC to normal write mode 0 */
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x00);
    outb(VGA_GC_INDEX, 0x01); outb(VGA_GC_DATA, 0x00);
}

/* -------------------------------------------------------------------------
 * Draw a single pixel at (x,y) with EGA color index `color` (0-15).
 *
 * VGA planar write mode 2: write the color index to the byte containing
 * the pixel; the GC bitmask register selects the exact bit.
 * ------------------------------------------------------------------------- */
static void vga_put_pixel(int x, int y, uint8_t color) {
    uint32_t byte_off = (uint32_t)y * SCREEN_STRIDE + (x >> 3);
    uint8_t  bit_mask = 0x80 >> (x & 7);

    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x02); /* write mode 2 */
    outb(VGA_GC_INDEX, 0x08); outb(VGA_GC_DATA, bit_mask); /* bit mask */

    (void)vga_peek(byte_off); /* latch the planes */
    vga_poke(byte_off, color);
}

/* -------------------------------------------------------------------------
 * NES -> EGA color mapping
 *
 * The original uses BGR565 values. We precompute a lookup from the 64
 * NES palette entries to the nearest EGA color index using simple
 * squared-distance in RGB space.
 *
 * NES BGR565 palette (same array as original, indices 0-63):
 * ------------------------------------------------------------------------- */
static const uint16_t nes_bgr565[64] = {
    25356, 34816, 39011, 30854, 24714, 4107,  106,   2311,
    2468,  2561,  4642,  6592,  20832, 0,     0,     0,
    44373, 49761, 55593, 51341, 43186, 18675, 434,   654,
    4939,  5058,  3074,  19362, 37667, 0,     0,     0,
    ~0,    ~819,  64497, 64342, 62331, 43932, 23612, 9465,
    1429,  1550,  20075, 36358, 52713, 16904, 0,     0,
    ~0,    ~328,  ~422,  ~452,  ~482,  58911, 50814, 42620,
    40667, 40729, 48951, 53078, 61238, 44405, 0,     0
};

/* Map one NES palette index (0-63) to EGA color index (0-15) */
static uint8_t nes_to_ega[64];

static void build_palette(void) {
    int i, j;
    for (i = 0; i < 64; i++) {
        uint16_t bgr = nes_bgr565[i];
        /* Extract R,G,B from BGR565 */
        int b5 = (bgr >> 11) & 0x1F;
        int g6 = (bgr >>  5) & 0x3F;
        int r5 = (bgr >>  0) & 0x1F;
        /* Scale to 0-63 range (same as EGA DAC entries) */
        int r = r5 * 2;   /* 5-bit -> ~6-bit */
        int g = g6;       /* already 6-bit */
        int b = b5 * 2;

        int best = 0, best_dist = 0x7FFFFFFF;
        for (j = 0; j < 16; j++) {
            int dr = r - (int)ega_dac[j][0];
            int dg = g - (int)ega_dac[j][1];
            int db = b - (int)ega_dac[j][2];
            int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) {
                best_dist = dist;
                best = j;
            }
        }
        nes_to_ega[i] = (uint8_t)best;
    }
}

/* -------------------------------------------------------------------------
 * Terminal / keyboard handling
 *
 * On ELKS we read /dev/tty in raw mode. We track key state with a simple
 * byte per key. For a proof of concept we poll non-blocking.
 *
 * NES button mapping:
 *   A      -> 'x'
 *   B      -> 'z'
 *   Select -> TAB  (0x09)
 *   Start  -> Enter (0x0D)
 *   Up     -> 'w' or ESC[A
 *   Down   -> 's' or ESC[B
 *   Left   -> 'a' or ESC[D
 *   Right  -> 'd' or ESC[C
 * ------------------------------------------------------------------------- */
static struct termios orig_termios;
static int tty_fd = -1;

/* Simple key state: 1 if held */
static uint8_t key_a, key_b, key_sel, key_start;
static uint8_t key_up, key_down, key_left, key_right;

static void tty_raw(void) {
    struct termios t;
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) tty_fd = 0; /* fallback to stdin */
    tcgetattr(tty_fd, &orig_termios);
    t = orig_termios;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN]  = 0;  /* non-blocking */
    t.c_cc[VTIME] = 0;
    tcsetattr(tty_fd, TCSAFLUSH, &t);
}

static void tty_restore(void) {
    if (tty_fd >= 0)
        tcsetattr(tty_fd, TCSAFLUSH, &orig_termios);
}

/* Poll keyboard, update key state */
static void poll_keys(void) {
    uint8_t buf[8];
    int n = read(tty_fd >= 0 ? tty_fd : 0, buf, sizeof(buf));
    int i;
    if (n <= 0) return;

    /* Clear state on each poll (keys must be held each frame) */
    key_a = key_b = key_sel = key_start = 0;
    key_up = key_down = key_left = key_right = 0;

    for (i = 0; i < n; i++) {
        uint8_t c = buf[i];
        switch (c) {
        case 'x': case 'X': key_a     = 1; break;
        case 'z': case 'Z': key_b     = 1; break;
        case '\t':           key_sel   = 1; break;
        case '\r': case '\n': key_start = 1; break;
        case 'w': case 'W': key_up    = 1; break;
        case 's': case 'S': key_down  = 1; break;
        case 'a': case 'A': key_left  = 1; break;
        case 'd': case 'D': key_right = 1; break;
        case 'q': case 'Q': case 27: /* ESC or q => quit */
            tty_restore();
            exit(0);
        }
        /* Handle ANSI arrow keys: ESC [ A/B/C/D */
        if (c == 27 && i + 2 < n && buf[i+1] == '[') {
            switch (buf[i+2]) {
            case 'A': key_up    = 1; break;
            case 'B': key_down  = 1; break;
            case 'C': key_right = 1; break;
            case 'D': key_left  = 1; break;
            }
            i += 2;
        }
    }
}

/* -------------------------------------------------------------------------
 * smolnes emulator core (unchanged from original except SDL references)
 * ------------------------------------------------------------------------- */

#define PULL   mem(++S, 1, 0, 0)
#define PUSH(x) mem(S--, 1, x, 1)

uint8_t *rom, *chrrom,
    prg[4], chr[8],
    prgbits = 14, chrbits = 12,
    A, X, Y, P = 4, S = ~2, PCH, PCL,
    addr_lo, addr_hi,
    nomem,
    result,
    val,
    cross,
    tmp,
    ppumask, ppuctrl, ppustatus,
    ppubuf,
    W,
    fine_x,
    opcode,
    nmi_irq,
    ntb,
    ptb_lo,
    vram[2048],
    palette_ram[64],
    ram[8192],
    chrram[8192],
    prgram[8192],
    oam[256],
    mask[] = {128, 64, 1, 2,
              1,   0,  0, 1, 4, 0, 0, 4, 0,
              0,   64, 0, 8, 0, 0, 8},
    keys,
    mirror,
    mmc1_bits, mmc1_data, mmc1_ctrl,
    mmc3_chrprg[8], mmc3_bits,
    mmc3_irq, mmc3_latch,
    chrbank0, chrbank1, prgbank,
    rombuf[1024 * 1024],
    *key_state_dummy; /* unused, kept for structure */

uint16_t scany,
    T, V,
    sum,
    dot,
    atb,
    shift_hi, shift_lo,
    cycles,
    frame_buffer[61440];  /* 256x240 NES framebuffer, BGR565 */

int shift_at = 0;

uint8_t *get_chr_byte(uint16_t a) {
    return &chrrom[chr[a >> chrbits] << chrbits | a % (1 << chrbits)];
}

uint8_t *get_nametable_byte(uint16_t a) {
    return &vram[mirror == 0   ? a % 1024
                 : mirror == 1 ? a % 1024 + 1024
                 : mirror == 2 ? a & 2047
                               : a / 2 & 1024 | a % 1024];
}

uint8_t mem(uint8_t lo, uint8_t hi, uint8_t val, uint8_t write) {
    uint16_t addr = hi << 8 | lo;

    switch (hi >>= 4) {
    case 0: case 1:
        return write ? ram[addr] = val : ram[addr];

    case 2: case 3:
        lo &= 7;
        if (lo == 7) {
            tmp = ppubuf;
            uint8_t *r =
                V < 8192 ? write && chrrom != chrram ? &tmp : get_chr_byte(V)
                : V < 16128 ? get_nametable_byte(V)
                            : palette_ram + (uint8_t)((V & 19) == 16 ? V ^ 16 : V);
            write ? *r = val : (ppubuf = *r);
            V += ppuctrl & 4 ? 32 : 1;
            V %= 16384;
            return tmp;
        }
        if (write)
            switch (lo) {
            case 0:
                ppuctrl = val;
                T = T & 0xf3ff | val % 4 << 10;
                break;
            case 1:
                ppumask = val;
                break;
            case 5:
                T = (W ^= 1)
                    ? fine_x = val & 7, T & ~31 | val / 8
                    : T & 0x8c1f | val % 8 << 12 | val * 4 & 0x3e0;
                break;
            case 6:
                T = (W ^= 1)
                    ? T & 0xff | val % 64 << 8
                    : (V = T & ~0xff | val);
            }
        if (lo == 2) {
            tmp = ppustatus & 0xe0;
            ppustatus &= 0x7f;
            W = 0;
            return tmp;
        }
        break;

    case 4:
        if (write && lo == 20)
            for (uint16_t i = 256; i--;)
                oam[i] = mem(i, val, 0, 0);
        /* Joypad: read keys from our state */
        {
            uint8_t k = 0;
            /* Build joypad byte: A,B,Sel,Start,Up,Down,Left,Right */
            k = (key_right << 7) | (key_left  << 6) |
                (key_down  << 5) | (key_up    << 4) |
                (key_start << 3) | (key_sel   << 2) |
                (key_b     << 1) | (key_a     << 0);
            tmp = k;
        }
        if (lo == 22) {
            if (write) {
                keys = tmp;
            } else {
                tmp = keys & 1;
                keys /= 2;
                return tmp;
            }
        }
        return 0;

    case 6: case 7:
        addr &= 8191;
        return write ? prgram[addr] = val : prgram[addr];

    default:
        if (write)
            switch (rombuf[6] >> 4) {
            case 7:
                mirror = !(val / 16);
                prg[0] = val % 8 * 2;
                prg[1] = prg[0] + 1;
                break;
            case 4: {
                uint8_t addr1 = addr & 1;
                switch (hi >> 1) {
                case 4:
                    *(addr1 ? &mmc3_chrprg[mmc3_bits & 7] : &mmc3_bits) = val;
                    tmp = mmc3_bits >> 5 & 4;
                    for (int ii = 4; ii--;) {
                        chr[0 + ii + tmp] = mmc3_chrprg[ii / 2] & ~!(ii % 2) | ii % 2;
                        chr[4 + ii - tmp] = mmc3_chrprg[2 + ii];
                    }
                    tmp = mmc3_bits >> 5 & 2;
                    prg[0 + tmp] = mmc3_chrprg[6];
                    prg[1] = mmc3_chrprg[7];
                    prg[3] = rombuf[4] * 2 - 1;
                    prg[2 - tmp] = prg[3] - 1;
                    break;
                case 5:
                    if (!addr1) mirror = 2 + val % 2;
                    break;
                case 6:
                    if (!addr1) mmc3_latch = val;
                    break;
                case 7:
                    mmc3_irq = addr1;
                    break;
                }
                break;
            }
            case 3:
                chr[0] = val % 4 * 2;
                chr[1] = chr[0] + 1;
                break;
            case 2:
                prg[0] = val & 31;
                break;
            case 1:
                if (val & 0x80) {
                    mmc1_bits = 5;
                    mmc1_data = 0;
                    mmc1_ctrl |= 12;
                } else if (mmc1_data = mmc1_data / 2 | val << 4 & 16, !--mmc1_bits) {
                    mmc1_bits = 5;
                    tmp = addr >> 13;
                    *(tmp == 4 ? mirror = mmc1_data & 3, &mmc1_ctrl
                      : tmp == 5 ? &chrbank0
                      : tmp == 6 ? &chrbank1
                                 : &prgbank) = mmc1_data;
                    chr[0] = chrbank0 & ~!(mmc1_ctrl & 16);
                    chr[1] = mmc1_ctrl & 16 ? chrbank1 : chrbank0 | 1;
                    tmp = mmc1_ctrl / 4 % 4 - 2;
                    prg[0] = !tmp ? 0 : tmp == 1 ? prgbank : prgbank & ~1;
                    prg[1] = !tmp ? prgbank : tmp == 1 ? rombuf[4] - 1 : prgbank | 1;
                }
            }
        return rom[(prg[hi - 8 >> prgbits - 12] & (rombuf[4] << 14 - prgbits) - 1)
                       << prgbits |
                   addr & (1 << prgbits) - 1];
    }
    return ~0;
}

uint8_t read_pc(void) {
    val = mem(PCL, PCH, 0, 0);
    !++PCL && ++PCH;
    return val;
}

uint8_t set_nz(uint8_t v) { return P = P & 125 | v & 128 | !v * 2; }

/* -------------------------------------------------------------------------
 * Render the NES frame_buffer to VGA
 *
 * frame_buffer[] is 256x240 BGR565. We skip the first 8 rows (garbage),
 * drawing rows 8..231 (224 rows). Each pixel maps to a 2x1 block on screen.
 *
 * We batch by scanline to minimize GC register writes:
 * For each screen row, build 4 bitplane bytes row by row.
 * ------------------------------------------------------------------------- */
static void render_frame(void) {
    int row, col;

    /* Switch to write mode 2 once; bit mask set per pixel */
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x02);

    for (row = 0; row < NES_H; row++) {
        int screen_y = NES_YOFF + row;
        /* NES framebuffer starts at row 8 (skip top garbage) */
        int nes_row = row + 8;
        uint16_t *src = &frame_buffer[nes_row * 256];

        for (col = 0; col < NES_W; col++) {
            /* Map BGR565 -> NES palette index -> EGA color */
            uint16_t bgr = src[col];
            /* Find which NES palette entry this is.
             * The frame_buffer stores the looked-up BGR565 directly,
             * so we do a reverse lookup into nes_bgr565[]. For speed,
             * we just do nearest-color match to EGA directly. */
            int b5 = (bgr >> 11) & 0x1F;
            int g6 = (bgr >>  5) & 0x3F;
            int r5 = (bgr >>  0) & 0x1F;
            int r = r5 * 2, g = g6, b = b5 * 2;
            int best = 0, best_dist = 0x7FFFFFFF, j;
            for (j = 0; j < 16; j++) {
                int dr = r - (int)ega_dac[j][0];
                int dg = g - (int)ega_dac[j][1];
                int db = b - (int)ega_dac[j][2];
                int dist = dr*dr + dg*dg + db*db;
                if (dist < best_dist) { best_dist = dist; best = j; }
            }
            uint8_t color = (uint8_t)best;

            /* Draw 2 pixels wide at (NES_XOFF + col*2, screen_y) */
            int sx0 = NES_XOFF + col * 2;
            int sx1 = sx0 + 1;

            /* Pixel 0 */
            {
                uint32_t boff = (uint32_t)screen_y * SCREEN_STRIDE + (sx0 >> 3);
                uint8_t  bm   = 0x80 >> (sx0 & 7);
                outb(VGA_GC_INDEX, 0x08); outb(VGA_GC_DATA, bm);
                (void)vga_peek(boff);
                vga_poke(boff, color);
            }
            /* Pixel 1 */
            {
                uint32_t boff = (uint32_t)screen_y * SCREEN_STRIDE + (sx1 >> 3);
                uint8_t  bm   = 0x80 >> (sx1 & 7);
                outb(VGA_GC_INDEX, 0x08); outb(VGA_GC_DATA, bm);
                (void)vga_peek(boff);
                vga_poke(boff, color);
            }
        }
    }

    /* Restore GC */
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x00);
    outb(VGA_GC_INDEX, 0x08); outb(VGA_GC_DATA, 0xFF);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    int fd;
    ssize_t n;

    if (argc < 2) {
        write(2, "Usage: smolnes <rom.nes>\n", 24);
        return 1;
    }

    /* Load ROM */
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    n = read(fd, rombuf, sizeof(rombuf));
    close(fd);
    if (n < 16) {
        write(2, "ROM too small\n", 14);
        return 1;
    }

    /* Initialize NES state (same as original) */
    rom = rombuf + 16;
    prg[1] = rombuf[4] - 1;
    chrrom = rombuf[5] ? rom + (rombuf[4] << 14) : chrram;
    chr[1] = rombuf[5] ? rombuf[5] * 2 - 1 : 1;
    mirror = 3 - rombuf[6] % 2;
    if (rombuf[6] / 16 == 4) {
        mem(0, 128, 0, 1);
        prgbits--;
        chrbits -= 2;
    }

    PCL = mem(~3, ~0, 0, 0);
    PCH = mem(~2, ~0, 0, 0);

    /* Build NES->EGA palette map */
    build_palette();

    /* Switch to raw terminal for keyboard */
    tty_raw();

    /* Initialize VGA mode 0x12 */
    vga_set_mode12();

loop:
    cycles = nomem = 0;
    if (nmi_irq)
        goto nmi_irq;

    opcode = read_pc();
    uint8_t opcodelo5 = opcode & 31;
    switch (opcodelo5) {
    case 0:
        if (opcode & 0x80) {
            read_pc();
            nomem = 1;
            goto nomemop;
        }
        switch (opcode >> 5) {
        case 0: {
            !++PCL && ++PCH;
        nmi_irq:
            PUSH(PCH);
            PUSH(PCL);
            PUSH(P | 32);
            uint16_t veclo = ~1 - (nmi_irq & 4);
            PCL = mem(veclo, ~0, 0, 0);
            PCH = mem(veclo + 1, ~0, 0, 0);
            nmi_irq = 0;
            cycles++;
            break;
        }
        case 1:
            result = read_pc();
            PUSH(PCH);
            PUSH(PCL);
            PCH = read_pc();
            PCL = result;
            break;
        case 2:
            P = PULL & ~32;
            PCL = PULL;
            PCH = PULL;
            break;
        case 3:
            PCL = PULL;
            PCH = PULL;
            !++PCL && ++PCH;
            break;
        }
        cycles += 4;
        break;

    case 16:
        read_pc();
        if (!(P & mask[opcode >> 6]) ^ opcode / 32 & 1) {
            cross = PCL + (int8_t)val >> 8;
            PCH += cross;
            PCL += val;
            cycles += cross ? 2 : 1;
        }
        break;

    case 8: case 24:
        switch (opcode >>= 4) {
        case 0:  PUSH(P | 48); cycles++; break;
        case 2:  P = PULL & ~16; cycles += 2; break;
        case 4:  PUSH(A); cycles++; break;
        case 6:  set_nz(A = PULL); cycles += 2; break;
        case 8:  set_nz(--Y); break;
        case 9:  set_nz(A = Y); break;
        case 10: set_nz(Y = A); break;
        case 12: set_nz(++Y); break;
        case 14: set_nz(++X); break;
        default: P = P & ~mask[opcode + 3] | mask[opcode + 4]; break;
        }
        break;

    case 10: case 26:
        switch (opcode >> 4) {
        case 8:  set_nz(A = X); break;
        case 9:  S = X; break;
        case 10: set_nz(X = A); break;
        case 11: set_nz(X = S); break;
        case 12: set_nz(--X); break;
        case 14: break; /* NOP */
        default:
            nomem = 1;
            val = A;
            goto nomemop;
        }
        break;

    case 1:
        read_pc();
        val += X;
        addr_lo = mem(val, 0, 0, 0);
        addr_hi = mem(val + 1, 0, 0, 0);
        cycles += 4;
        goto opcode;

    case 2: case 9:
        read_pc();
        nomem = 1;
        goto nomemop;

    case 17:
        addr_lo = mem(read_pc(), 0, 0, 0);
        addr_hi = mem(val + 1, 0, 0, 0);
        cycles++;
        goto add_x_or_y;

    case 4: case 5: case 6:
    case 20: case 21: case 22:
        addr_lo = read_pc();
        cross = opcodelo5 > 6;
        if (cross)
            addr_lo += (opcode & 214) == 150 ? Y : X;
        addr_hi = 0;
        cycles -= !cross;
        goto opcode;

    case 12: case 13: case 14:
    case 25:
    case 28: case 29: case 30:
        addr_lo = read_pc();
        addr_hi = read_pc();
        if (opcodelo5 < 25) goto opcode;
    add_x_or_y:
        val = opcodelo5 < 28 | opcode == 190 ? Y : X;
        cross = addr_lo + val > 255;
        addr_hi += cross;
        addr_lo += val;
        cycles += ((opcode & 224) == 128 | opcode % 16 == 14 & opcode != 190) | cross;
    opcode:
        cycles += 2;
        if (opcode != 76 & (opcode & 224) != 128)
            val = mem(addr_lo, addr_hi, 0, 0);
        break;
    }
    /* fall through to ALU/nomemop */

nomemop:
    result = 0;
    switch (opcode & 227) {
    case 1:   set_nz(A |= val); break;
    case 33:  set_nz(A &= val); break;
    case 65:  set_nz(A ^= val); break;
    case 225: val = ~val; /* fallthrough */
    case 97:
        sum = A + val + P % 2;
        P = P & ~65 | sum > 255 | ((A ^ sum) & (val ^ sum) & 128) / 2;
        set_nz(A = sum);
        break;
    case 34:  result = P & 1; /* fallthrough */
    case 2:
        result |= val * 2;
        P = P & ~1 | val / 128;
        goto memop;
    case 98:  result = P << 7; /* fallthrough */
    case 66:
        result |= val / 2;
        P = P & ~1 | val & 1;
        goto memop;
    case 194: result = val - 1; goto memop;
    case 226: result = val + 1; /* fallthrough */
    memop:
        set_nz(result);
        nomem ? A = result : (cycles += 2, mem(addr_lo, addr_hi, result, 1));
        break;
    case 32:
        P = P & 61 | val & 192 | !(A & val) * 2;
        break;
    case 64:
        PCL = addr_lo;
        PCH = addr_hi;
        cycles--;
        break;
    case 96:
        PCL = val;
        PCH = mem(addr_lo + 1, addr_hi, 0, 0);
        cycles++;
        break;
    default: {
        uint8_t opcodehi3 = opcode / 32;
        uint8_t *reg = opcode % 4 == 2 | opcodehi3 == 7 ? &X
                       : opcode % 4 == 1                  ? &A
                                                          : &Y;
        if (opcodehi3 == 4)
            mem(addr_lo, addr_hi, *reg, 1);
        else if (opcodehi3 != 5) {
            P = P & ~1 | *reg >= val;
            set_nz(*reg - val);
        } else
            set_nz(*reg = val);
        break;
    }
    }

    /* PPU update */
    for (tmp = cycles * 3 + 6; tmp--;) {
        if (ppumask & 24) {
            if (scany < 240) {
                if (dot - 256 > 63u) {
                    if (dot < 256) {
                        uint8_t color = shift_hi >> 14 - fine_x & 2 |  /* I swear I am not Yandere Dev -vmunix */
                                        shift_lo >> 15 - fine_x & 1,
                                palette = shift_at >> 28 - fine_x * 2 & 12;

                        if (ppumask & 16) {
                            uint8_t *sprite;
                            for (sprite = oam; sprite < oam + 256; sprite += 4) {
                                uint16_t sprite_h = ppuctrl & 32 ? 16 : 8,
                                         sprite_x = dot - sprite[3],
                                         sprite_y = scany - sprite[0] - 1,
                                         sx = sprite_x ^ !(sprite[2] & 64) * 7,
                                         sy = sprite_y ^ (sprite[2] & 128 ? sprite_h - 1 : 0);
                                if (sprite_x < 8 && sprite_y < sprite_h) {
                                    uint16_t sprite_tile = sprite[1],
                                             sprite_addr =
                                                 (ppuctrl & 32
                                                      ? sprite_tile % 2 << 12 |
                                                            sprite_tile << 4 & -32 | sy * 2 & 16
                                                      : (ppuctrl & 8) << 9 | sprite_tile << 4) |
                                                 sy & 7,
                                             sprite_color =
                                                 *get_chr_byte(sprite_addr + 8) >> sx << 1 & 2 |
                                                 *get_chr_byte(sprite_addr) >> sx & 1;
                                    if (sprite_color) {
                                        if (!(sprite[2] & 32 && color)) {
                                            color = sprite_color;
                                            palette = 16 | sprite[2] * 4 & 12;
                                        }
                                        if (sprite == oam && color)
                                            ppustatus |= 64;
                                        break;
                                    }
                                }
                            }
                        }

                        frame_buffer[scany * 256 + dot] =
                            (uint16_t[64]){
                                25356, 34816, 39011, 30854, 24714, 4107,  106,   2311,
                                2468,  2561,  4642,  6592,  20832, 0,     0,     0,
                                44373, 49761, 55593, 51341, 43186, 18675, 434,   654,
                                4939,  5058,  3074,  19362, 37667, 0,     0,     0,
                                ~0,    ~819,  64497, 64342, 62331, 43932, 23612, 9465,
                                1429,  1550,  20075, 36358, 52713, 16904, 0,     0,
                                ~0,    ~328,  ~422,  ~452,  ~482,  58911, 50814, 42620,
                                40667, 40729, 48951, 53078, 61238, 44405}
                                [palette_ram[color ? palette | color : 0]];
                    }

                    if (dot < 336) {
                        shift_hi *= 2;
                        shift_lo *= 2;
                        shift_at *= 4;
                    }

                    int temp = ppuctrl << 8 & 4096 | ntb << 4 | V >> 12;
                    switch (dot & 7) {
                    case 1: ntb = *get_nametable_byte(V); break;
                    case 3:
                        atb = (*get_nametable_byte(V & 0xc00 | 0x3c0 | V >> 4 & 0x38 |
                                                   V / 4 & 7) >>
                               (V >> 5 & 2 | V / 2 & 1) * 2) % 4 * 0x5555;
                        break;
                    case 5: ptb_lo = *get_chr_byte(temp); break;
                    case 7: {
                        uint8_t ptb_hi = *get_chr_byte(temp | 8);
                        V = V % 32 == 31 ? V & ~31 ^ 1024 : V + 1;
                        shift_hi |= ptb_hi;
                        shift_lo |= ptb_lo;
                        shift_at |= atb;
                        break;
                    }
                    }
                }

                if (dot == 256) {
                    V = ((V & 7 << 12) != 7 << 12 ? V + 4096
                         : (V & 0x3e0) == 928     ? V & 0x8c1f ^ 2048
                         : (V & 0x3e0) == 0x3e0   ? V & 0x8c1f
                                                   : V & 0x8c1f | V + 32 & 0x3e0) &
                            ~0x41f |
                        T & 0x41f;
                }
            }

            if ((scany + 1) % 262 < 241 && dot == 261 && mmc3_irq && !mmc3_latch--)
                nmi_irq = 1;

            if (scany == 261 && dot - 280 < 25u)
                V = V & 0x841f | T & 0x7be0;
        }

        if (dot == 1) {
            if (scany == 241) {
                if (ppuctrl & 128)
                    nmi_irq = 4;
                ppustatus |= 128;

                /* --- RENDER FRAME TO VGA --- */
                poll_keys();
                render_frame();
            }
            if (scany == 261)
                ppustatus = 0;
        }

        if (++dot == 341) {
            dot = 0;
            scany++;
            scany %= 262;
        }
    }
    goto loop;
}
