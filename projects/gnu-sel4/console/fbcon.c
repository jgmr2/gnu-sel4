/*
 * fbcon: the GNU/seL4 graphical console. To the rest of the system it is the
 * sDDF "serial driver" (same queues as the UART), so posix_server's terminal
 * and switching to the VM console work the same; but instead of a serial port
 * it uses the screen and the keyboard:
 *
 *   output   tx queue -> terminal emulator (term.c) -> framebuffer
 *   input    PS/2 keyboard (i8042, IRQ 1) -> bytes like xterm's -> rx queue
 *
 * Screen: QEMU bochs-display (VBE "dispi" registers over MMIO, 32-bit linear
 * framebuffer). Font: Spleen 8x16. Keyboard: US layout.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdbool.h>
#include <stdint.h>
#include <microkit.h>
#include <sddf/serial/queue.h>
#include <sddf/serial/config.h>
#include "term.h"
#include "font_spleen.h"

__attribute__((__section__(".serial_driver_config"))) serial_driver_config_t config;

/* Regions and resources (meta.py) */
uintptr_t fb_base;                  /* linear framebuffer */
uintptr_t fb_regs;                  /* bochs-display MMIO registers */
#define KBD_IRQ_CH   10
#define IOPORT_DATA  0              /* 0x60 */
#define IOPORT_CMD   1              /* 0x64 */

#define FB_W 1280
#define FB_H 800

static serial_queue_handle_t rx_q, tx_q;

/* ---------------- screen ---------------- */

/* Bochs VBE "dispi": 16-bit register i at fb_regs + 0x500 + 2 * i */
enum { VBE_XRES = 1, VBE_YRES, VBE_BPP, VBE_ENABLE, VBE_BANK, VBE_VIRT_W, VBE_VIRT_H, VBE_XOFF, VBE_YOFF };
static void vbe(int i, uint16_t v) { *(volatile uint16_t *)(fb_regs + 0x500 + 2 * i) = v; }

static void display_init(void)
{
    vbe(VBE_ENABLE, 0);
    vbe(VBE_XRES, FB_W);
    vbe(VBE_YRES, FB_H);
    vbe(VBE_BPP, 32);
    vbe(VBE_VIRT_W, FB_W);
    vbe(VBE_VIRT_H, FB_H);
    vbe(VBE_XOFF, 0);
    vbe(VBE_YOFF, 0);
    vbe(VBE_ENABLE, 0x01 | 0x40);          /* enabled, linear framebuffer */
}

static const uint8_t *glyph(uint32_t ch)
{
    int lo = 0, hi = FONT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cp[mid] == ch) return font_bits[mid];
        if (font_cp[mid] < ch) lo = mid + 1; else hi = mid - 1;
    }
    return ch == '?' ? font_bits[0] : glyph('?');
}

void fb_draw_cell(int col, int row, uint32_t ch, uint32_t fg, uint32_t bg, int attrs)
{
    if ((attrs & ATTR_REVERSE) ^ ((attrs & ATTR_CURSOR) ? ATTR_REVERSE : 0)) {
        uint32_t t = fg; fg = bg; bg = t;
    }
    const uint8_t *g = glyph(ch);
    volatile uint32_t *p = (volatile uint32_t *)fb_base + (size_t)row * FONT_H * FB_W + (size_t)col * FONT_W;
    for (int y = 0; y < FONT_H; y++, p += FB_W) {
        uint8_t bits = g[y];
        if ((attrs & ATTR_UNDERLINE) && y == FONT_H - 1) bits = 0xff;
        for (int x = 0; x < FONT_W; x++) p[x] = (bits & (0x80 >> x)) ? fg : bg;
    }
}

/* ---------------- sDDF queues ---------------- */

static bool rx_sent;

static void send(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (serial_queue_full(&rx_q, rx_q.queue->tail)) break;
        serial_enqueue(&rx_q, s[i]);
        rx_sent = true;
    }
}

void term_reply(const char *s, size_t n) { send(s, n); }

static void flush_rx(void)
{
    if (rx_sent) microkit_notify(config.rx.id);
    rx_sent = false;
}

static void tx_drain(void)
{
    uint8_t buf[512];
    bool got = false;
    for (;;) {
        size_t n = 0;
        char c;
        while (n < sizeof buf && !serial_dequeue(&tx_q, &c)) buf[n++] = c;
        if (!n) break;
        term_input(buf, n);
        got = true;
    }
    if (got) {
        term_render();
        if (serial_require_consumer_signal(&tx_q)) {
            serial_cancel_consumer_signal(&tx_q);
            microkit_notify(config.tx.id);
        }
    }
    flush_rx();                             /* terminal replies (DA, CPR) */
}

/* ---------------- PS/2 keyboard (set 1, translated by the i8042) ---------------- */

static const char kmap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 127, '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.',
};
static const char kmap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 127, '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.',
};

static bool shift_l, shift_r, ctrl, alt, caps, e0;

static void key_seq(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    send(s, n);
}

static void key(uint8_t sc)
{
    if (sc == 0xe0) { e0 = true; return; }
    bool up = sc & 0x80, ext = e0;
    sc &= 0x7f;
    e0 = false;

    switch (sc) {                               /* modifiers */
    case 0x2a: if (!ext) shift_l = !up; return;
    case 0x36: shift_r = !up; return;
    case 0x1d: ctrl = !up; return;
    case 0x38: alt = !up; return;
    case 0x3a: if (!up) caps = !caps; return;
    }
    if (up) return;

    if (ext) {                                  /* editing keys and arrows */
        const char *s = NULL;
        bool app = term_app_cursor();
        switch (sc) {
        case 0x48: s = app ? "\033OA" : "\033[A"; break;
        case 0x50: s = app ? "\033OB" : "\033[B"; break;
        case 0x4d: s = app ? "\033OC" : "\033[C"; break;
        case 0x4b: s = app ? "\033OD" : "\033[D"; break;
        case 0x47: s = app ? "\033OH" : "\033[H"; break;
        case 0x4f: s = app ? "\033OF" : "\033[F"; break;
        case 0x49: s = "\033[5~"; break;
        case 0x51: s = "\033[6~"; break;
        case 0x52: s = "\033[2~"; break;
        case 0x53: s = "\033[3~"; break;
        case 0x1c: s = "\r"; break;              /* keypad Enter */
        case 0x35: s = "/"; break;
        }
        if (s) key_seq(s);
        return;
    }
    static const char *const fkeys[12] = {
        "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
        "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
    };
    if (sc >= 0x3b && sc <= 0x44) { key_seq(fkeys[sc - 0x3b]); return; }
    if (sc == 0x57 || sc == 0x58) { key_seq(fkeys[10 + sc - 0x57]); return; }

    bool shift = shift_l || shift_r;
    char c = (shift ? kmap_shift : kmap)[sc];
    if (!c) return;
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    else if (caps && c >= 'A' && c <= 'Z' && shift) c += 32;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c -= 'a' - 1;
        else if (c >= 'A' && c <= 'Z') c -= 'A' - 1;
        else if (c == ' ' || c == '2' || c == '@') c = 0;
        else if (c == '[' || c == '3') c = 27;
        else if (c == '\\' || c == '4') c = 28;
        else if (c == ']' || c == '5') c = 29;
        else if (c == '6' || c == '^') c = 30;
        else if (c == '/' || c == '-' || c == '_' || c == '7') c = 31;
        else if (c == '8' || c == '?') c = 127;
    }
    if (alt) send("\033", 1);                   /* Alt = ESC prefix, like xterm */
    send(&c, 1);
}

static void kbd_irq(void)
{
    while (microkit_x86_ioport_read_8(IOPORT_CMD, 0x64) & 1)       /* data available */
        key(microkit_x86_ioport_read_8(IOPORT_DATA, 0x60));
    flush_rx();
}

static void kbd_init(void)
{
    while (microkit_x86_ioport_read_8(IOPORT_CMD, 0x64) & 1)       /* drain */
        microkit_x86_ioport_read_8(IOPORT_DATA, 0x60);
    /* Controller configuration: keyboard interrupt and translation to set 1 */
    microkit_x86_ioport_write_8(IOPORT_CMD, 0x64, 0x20);
    for (int i = 0; i < 100000 && !(microkit_x86_ioport_read_8(IOPORT_CMD, 0x64) & 1); i++);
    uint8_t cfg = microkit_x86_ioport_read_8(IOPORT_DATA, 0x60);
    cfg |= 0x01 | 0x40;
    microkit_x86_ioport_write_8(IOPORT_CMD, 0x64, 0x60);
    for (int i = 0; i < 100000 && (microkit_x86_ioport_read_8(IOPORT_CMD, 0x64) & 2); i++);
    microkit_x86_ioport_write_8(IOPORT_DATA, 0x60, cfg);
}

/* ---------------- Microkit ---------------- */

void init(void)
{
    serial_queue_init(&rx_q, config.rx.queue.vaddr, config.rx.data.size, config.rx.data.vaddr);
    serial_queue_init(&tx_q, config.tx.queue.vaddr, config.tx.data.size, config.tx.data.vaddr);
    display_init();
    term_init(FB_W / FONT_W, FB_H / FONT_H);
    term_render();
    kbd_init();
}

void notified(microkit_channel ch)
{
    if (ch == config.tx.id) {
        tx_drain();
    } else if (ch == KBD_IRQ_CH) {
        kbd_irq();
        microkit_irq_ack(KBD_IRQ_CH);
    } else if (ch == config.rx.id) {
        flush_rx();
    }
}
