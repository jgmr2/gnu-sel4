/*
 * Terminal emulator of the graphical console: interprets program output like
 * an xterm (TERM=xterm-256color) on a grid of cells and only draws the cells
 * that change.
 *
 *   UTF-8; 16, 256 and 24-bit colours; bold, underline, reverse
 *   cursor movement, erase (with the current background colour, "bce"),
 *   insert and delete of characters and lines, scroll region,
 *   insert mode, deferred line wrap like xterm, tab stops
 *   alternate screen (vim, top, less), DECCKM, DEC Special Graphics (borders)
 *   replies: DA, secondary DA, DSR, cursor position (used by `resize`)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include "term.h"

typedef struct {
    uint32_t ch, fg, bg;
    uint8_t attr;
} cell_t;

#define DEF_FG 0xd0d0d0
#define DEF_BG 0x141414

/* 16-colour palette (the 8 normal ones and the 8 bright ones) */
static const uint32_t pal16[16] = {
    0x1c1c1c, 0xcc3333, 0x4e9a06, 0xc4a000, 0x3465a4, 0x9b59b6, 0x06989a, 0xd3d7cf,
    0x555753, 0xef2929, 0x8ae234, 0xfce94f, 0x729fcf, 0xc77ddb, 0x34e2e2, 0xeeeeec,
};

static uint32_t pal256(int i)
{
    if (i < 16) return pal16[i];
    if (i < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        i -= 16;
        return lv[i / 36] << 16 | lv[i / 6 % 6] << 8 | lv[i % 6];
    }
    uint32_t g = 8 + (i - 232) * 10;
    return g << 16 | g << 8 | g;
}

static int cols, rows;
static cell_t screens[2][TERM_MAX_ROWS][TERM_MAX_COLS];
static cell_t shown[TERM_MAX_ROWS][TERM_MAX_COLS];
static int scr;                                   /* 0 = main, 1 = alternate */
#define G screens[scr]

/* Pen: the attributes used for writing */
static struct pen {
    int fg_idx;                                   /* -1 = default, -2 = 24-bit */
    uint32_t fg, bg;
    uint8_t attr;
} pen;

static int cx, cy;                                /* cursor */
static bool wrap_pending, cursor_on = true, autowrap = true, insert_mode, app_cursor, origin;
static int top, bottom;                           /* scroll region (inclusive) */
static bool tabs[TERM_MAX_COLS];
static int charset[2], gl;                        /* G0/G1: 0 = ASCII, 1 = DEC Special Graphics */

static struct saved {
    int cx, cy;
    struct pen pen;
    bool origin;
    int charset[2], gl;
} saved[2];

bool term_app_cursor(void) { return app_cursor; }

/* ---------------- grid operations ---------------- */

static uint32_t pen_fg(void)
{
    if (pen.fg_idx >= 0 && pen.fg_idx < 8 && (pen.attr & ATTR_BOLD)) return pal16[pen.fg_idx + 8];
    return pen.fg;
}

static void blank(cell_t *c)
{
    c->ch = ' ';
    c->fg = pen_fg();
    c->bg = pen.bg;
    c->attr = 0;
}

static void clear_cells(int r, int c0, int c1)    /* [c0, c1) */
{
    for (int c = c0; c < c1; c++) blank(&G[r][c]);
}

static void scroll_up(int t, int b, int n)
{
    if (n > b - t + 1) n = b - t + 1;
    memmove(&G[t], &G[t + n], (size_t)(b - t + 1 - n) * sizeof G[0]);
    for (int r = b - n + 1; r <= b; r++) clear_cells(r, 0, cols);
}

static void scroll_down(int t, int b, int n)
{
    if (n > b - t + 1) n = b - t + 1;
    memmove(&G[t + n], &G[t], (size_t)(b - t + 1 - n) * sizeof G[0]);
    for (int r = t; r < t + n; r++) clear_cells(r, 0, cols);
}

static void clamp(void)
{
    if (cx < 0) cx = 0;
    if (cx >= cols) cx = cols - 1;
    int lo = origin ? top : 0, hi = origin ? bottom : rows - 1;
    if (cy < lo) cy = lo;
    if (cy > hi) cy = hi;
    wrap_pending = false;
}

static void index_down(void)                      /* LF / IND */
{
    if (cy == bottom) scroll_up(top, bottom, 1);
    else if (cy < rows - 1) cy++;
}

static void index_up(void)                        /* RI */
{
    if (cy == top) scroll_down(top, bottom, 1);
    else if (cy > 0) cy--;
}

/* DEC Special Graphics: 0x5f..0x7e -> Unicode (ncurses, top, vim borders) */
static const uint16_t dec_graphics[32] = {
    0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
    0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
    0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
    0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static void put_char(uint32_t ch)
{
    if (charset[gl] == 1 && ch >= 0x5f && ch <= 0x7e) ch = dec_graphics[ch - 0x5f];
    if (wrap_pending) {
        wrap_pending = false;
        cx = 0;
        index_down();
    }
    if (insert_mode)
        memmove(&G[cy][cx + 1], &G[cy][cx], (size_t)(cols - cx - 1) * sizeof(cell_t));
    cell_t *c = &G[cy][cx];
    c->ch = ch;
    c->fg = pen_fg();
    c->bg = pen.bg;
    c->attr = pen.attr;
    if (cx == cols - 1) wrap_pending = autowrap;
    else cx++;
}

static void reset(void)
{
    pen = (struct pen){ .fg_idx = -1, .fg = DEF_FG, .bg = DEF_BG };
    cx = cy = 0;
    top = 0;
    bottom = rows - 1;
    wrap_pending = insert_mode = app_cursor = origin = false;
    cursor_on = autowrap = true;
    charset[0] = charset[1] = gl = 0;
    for (int c = 0; c < TERM_MAX_COLS; c++) tabs[c] = c % 8 == 0;
    for (int s = 0; s < 2; s++)
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                screens[s][r][c] = (cell_t){ ' ', DEF_FG, DEF_BG, 0 };
            }
}

static void save_cursor(void)
{
    saved[scr] = (struct saved){ cx, cy, pen, origin, { charset[0], charset[1] }, gl };
}

static void restore_cursor(void)
{
    struct saved *s = &saved[scr];
    cx = s->cx;
    cy = s->cy;
    pen = s->pen;
    origin = s->origin;
    charset[0] = s->charset[0];
    charset[1] = s->charset[1];
    gl = s->gl;
    clamp();
}

static void alt_screen(bool on, bool save)
{
    if (on == (scr == 1)) return;
    if (on) {
        if (save) save_cursor();
        scr = 1;
        for (int r = 0; r < rows; r++) clear_cells(r, 0, cols);
    } else {
        scr = 0;
        if (save) restore_cursor();
    }
}

/* ---------------- sequences ---------------- */

static void reply(const char *s) { term_reply(s, strlen(s)); }

static void put_num(char **o, int v)
{
    char t[12];
    int n = 0;
    do t[n++] = '0' + v % 10; while ((v /= 10));
    while (n) *(*o)++ = t[--n];
}

static void sgr(const int *p, int n)
{
    if (!n) { n = 1; p = (const int[]){ 0 }; }
    for (int i = 0; i < n; i++) {
        int v = p[i];
        if (v == 0) pen = (struct pen){ .fg_idx = -1, .fg = DEF_FG, .bg = DEF_BG };
        else if (v == 1) pen.attr |= ATTR_BOLD;
        else if (v == 4) pen.attr |= ATTR_UNDERLINE;
        else if (v == 7) pen.attr |= ATTR_REVERSE;
        else if (v == 22) pen.attr &= ~ATTR_BOLD;
        else if (v == 24) pen.attr &= ~ATTR_UNDERLINE;
        else if (v == 27) pen.attr &= ~ATTR_REVERSE;
        else if (v >= 30 && v <= 37) { pen.fg_idx = v - 30; pen.fg = pal16[v - 30]; }
        else if (v == 39) { pen.fg_idx = -1; pen.fg = DEF_FG; }
        else if (v >= 40 && v <= 47) pen.bg = pal16[v - 40];
        else if (v == 49) pen.bg = DEF_BG;
        else if (v >= 90 && v <= 97) { pen.fg_idx = v - 90 + 8; pen.fg = pal16[v - 90 + 8]; }
        else if (v >= 100 && v <= 107) pen.bg = pal16[v - 100 + 8];
        else if ((v == 38 || v == 48) && i + 1 < n) {
            uint32_t col;
            if (p[i + 1] == 5 && i + 2 < n) {
                col = pal256(p[i + 2] & 255);
                if (v == 38) pen.fg_idx = p[i + 2] < 16 ? p[i + 2] : -2;
                i += 2;
            } else if (p[i + 1] == 2 && i + 4 < n) {
                col = (p[i + 2] & 255) << 16 | (p[i + 3] & 255) << 8 | (p[i + 4] & 255);
                if (v == 38) pen.fg_idx = -2;
                i += 4;
            } else {
                break;
            }
            if (v == 38) pen.fg = col; else pen.bg = col;
        }
    }
}

static void set_mode(int priv, const int *p, int n, bool on)
{
    for (int i = 0; i < n; i++) {
        if (!priv) {
            if (p[i] == 4) insert_mode = on;
            continue;
        }
        switch (p[i]) {
        case 1:    app_cursor = on; break;
        case 6:    origin = on; cx = 0; cy = on ? top : 0; clamp(); break;
        case 7:    autowrap = on; break;
        case 25:   cursor_on = on; break;
        case 47:
        case 1047: alt_screen(on, false); break;
        case 1048: if (on) save_cursor(); else restore_cursor(); break;
        case 1049: alt_screen(on, true); break;
        }
    }
}

static void csi(int priv, int inter, const int *p, int n, char f)
{
#define P(i, d) (n > (i) && p[i] ? p[i] : (d))
    int a = P(0, 1);
    char out[48], *o;
    switch (f) {
    case '@': {                                   /* ICH */
        if (a > cols - cx) a = cols - cx;
        memmove(&G[cy][cx + a], &G[cy][cx], (size_t)(cols - cx - a) * sizeof(cell_t));
        clear_cells(cy, cx, cx + a);
        wrap_pending = false;
        break;
    }
    case 'A': {                                   /* CUU: stops at the edge of the region */
        int lo = cy >= top ? top : 0;
        cy = cy - a < lo ? lo : cy - a;
        clamp();
        break;
    }
    case 'B': case 'e': {                         /* CUD, VPR */
        int hi = cy <= bottom ? bottom : rows - 1;
        cy = cy + a > hi ? hi : cy + a;
        clamp();
        break;
    }
    case 'C': case 'a': cx += a; clamp(); break;
    case 'D': cx -= a; clamp(); break;
    case 'E': cx = 0; cy += a; clamp(); break;
    case 'F': cx = 0; cy -= a; clamp(); break;
    case 'G': case '`': cx = a - 1; clamp(); break;
    case 'H': case 'f': cy = P(0, 1) - 1 + (origin ? top : 0); cx = P(1, 1) - 1; clamp(); break;
    case 'I': while (a--) { do cx++; while (cx < cols - 1 && !tabs[cx]); } clamp(); break;
    case 'Z': while (a--) { do cx--; while (cx > 0 && !tabs[cx]); } clamp(); break;
    case 'J': {
        int m = n ? p[0] : 0;
        if (m == 0) { clear_cells(cy, cx, cols); for (int r = cy + 1; r < rows; r++) clear_cells(r, 0, cols); }
        else if (m == 1) { clear_cells(cy, 0, cx + 1); for (int r = 0; r < cy; r++) clear_cells(r, 0, cols); }
        else for (int r = 0; r < rows; r++) clear_cells(r, 0, cols);
        wrap_pending = false;
        break;
    }
    case 'K': {
        int m = n ? p[0] : 0;
        if (m == 0) clear_cells(cy, cx, cols);
        else if (m == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, 0, cols);
        wrap_pending = false;
        break;
    }
    case 'L': if (cy >= top && cy <= bottom) scroll_down(cy, bottom, a); cx = 0; wrap_pending = false; break;
    case 'M': if (cy >= top && cy <= bottom) scroll_up(cy, bottom, a); cx = 0; wrap_pending = false; break;
    case 'P': {                                   /* DCH */
        if (a > cols - cx) a = cols - cx;
        memmove(&G[cy][cx], &G[cy][cx + a], (size_t)(cols - cx - a) * sizeof(cell_t));
        clear_cells(cy, cols - a, cols);
        wrap_pending = false;
        break;
    }
    case 'S': scroll_up(top, bottom, a); break;
    case 'T': if (!priv) scroll_down(top, bottom, a); break;
    case 'X': clear_cells(cy, cx, cx + a > cols ? cols : cx + a); wrap_pending = false; break;
    case 'b': if (cx > 0) { uint32_t ch = G[cy][cx - 1].ch; while (a--) put_char(ch); } break;
    case 'c':
        if (priv == '>') reply("\033[>0;0;0c");
        else if (!priv) reply("\033[?1;2c");
        break;
    case 'd': cy = a - 1 + (origin ? top : 0); clamp(); break;
    case 'g': if (P(0, 0) == 3) memset(tabs, 0, sizeof tabs); else tabs[cx] = false; break;
    case 'h': set_mode(priv, p, n, true); break;
    case 'l': set_mode(priv, p, n, false); break;
    case 'm': if (!priv && !inter) sgr(p, n); break;
    case 'n':
        if (priv) break;
        if (P(0, 0) == 5) reply("\033[0n");
        else if (P(0, 0) == 6) {
            o = out;
            *o++ = 033; *o++ = '[';
            put_num(&o, cy + 1 - (origin ? top : 0)); *o++ = ';'; put_num(&o, cx + 1); *o++ = 'R'; *o = 0;
            reply(out);
        }
        break;
    case 'p': if (inter == '!') reset(); break;     /* DECSTR */
    case 'r':
        if (priv) break;
        top = P(0, 1) - 1;
        bottom = P(1, rows) - 1;
        if (top < 0) top = 0;
        if (bottom >= rows) bottom = rows - 1;
        if (top >= bottom) { top = 0; bottom = rows - 1; }
        cx = 0;
        cy = origin ? top : 0;
        wrap_pending = false;
        break;
    case 's': if (!priv) save_cursor(); break;
    case 'u': if (!priv) restore_cursor(); break;
    case 't':
        if (P(0, 0) == 18) {                      /* size in characters */
            o = out;
            *o++ = 033; *o++ = '['; *o++ = '8'; *o++ = ';';
            put_num(&o, rows); *o++ = ';'; put_num(&o, cols); *o++ = 't'; *o = 0;
            reply(out);
        }
        break;
    }
#undef P
}

/* ---------------- parser ---------------- */

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_STR, S_CHARSET };
static int state, utf_left;
static uint32_t utf_cp;
static int params[16], nparams, priv_ch, inter_ch, cs_target;
static bool str_esc;

static void control(uint8_t c)
{
    switch (c) {
    case 7: break;                                /* BEL */
    case 8: if (cx > 0) cx--; wrap_pending = false; break;
    case 9: do cx++; while (cx < cols - 1 && !tabs[cx]); if (cx >= cols) cx = cols - 1; wrap_pending = false; break;
    case 10: case 11: case 12: index_down(); wrap_pending = false; break;
    case 13: cx = 0; wrap_pending = false; break;
    case 14: gl = 1; break;                       /* SO */
    case 15: gl = 0; break;                       /* SI */
    case 27: state = S_ESC; break;
    }
}

static void byte(uint8_t c)
{
    switch (state) {
    case S_GROUND:
        if (utf_left) {
            if ((c & 0xc0) == 0x80) {
                utf_cp = utf_cp << 6 | (c & 0x3f);
                if (--utf_left == 0) put_char(utf_cp);
                return;
            }
            utf_left = 0;
            put_char(0xfffd);
        }
        if (c < 0x20 || c == 0x7f) { if (c != 0x7f) control(c); return; }
        if (c < 0x80) { put_char(c); return; }
        if ((c & 0xe0) == 0xc0) { utf_cp = c & 0x1f; utf_left = 1; }
        else if ((c & 0xf0) == 0xe0) { utf_cp = c & 0x0f; utf_left = 2; }
        else if ((c & 0xf8) == 0xf0) { utf_cp = c & 0x07; utf_left = 3; }
        else put_char(0xfffd);
        return;
    case S_ESC:
        state = S_GROUND;
        switch (c) {
        case '[': state = S_CSI; nparams = 0; priv_ch = inter_ch = 0; memset(params, 0, sizeof params); break;
        case ']': state = S_OSC; str_esc = false; break;
        case 'P': case '_': case '^': state = S_STR; str_esc = false; break;
        case '(': case ')': state = S_CHARSET; cs_target = c == ')'; break;
        case '7': save_cursor(); break;
        case '8': restore_cursor(); break;
        case 'D': index_down(); break;
        case 'E': cx = 0; index_down(); break;
        case 'H': tabs[cx] = true; break;
        case 'M': index_up(); wrap_pending = false; break;
        case 'c': reset(); break;
        }
        return;
    case S_CHARSET:
        charset[cs_target] = c == '0';
        state = S_GROUND;
        return;
    case S_CSI:
        if (c >= '0' && c <= '9') {
            if (!nparams) nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (!nparams) nparams = 1;
            if (nparams < 16) params[nparams++] = 0;
        } else if (c >= '<' && c <= '?') {
            priv_ch = c;
        } else if (c >= 0x20 && c <= 0x2f) {
            inter_ch = c;
        } else if (c >= 0x40 && c <= 0x7e) {
            csi(priv_ch, inter_ch, params, nparams, c);
            state = S_GROUND;
        } else if (c < 0x20) {
            control(c);                           /* controls inside CSI are executed */
        } else {
            state = S_GROUND;
        }
        return;
    case S_OSC:                                   /* window titles, colours: ignored */
    case S_STR:
        if (c == 7 && state == S_OSC) state = S_GROUND;
        else if (str_esc && c == '\\') state = S_GROUND;
        str_esc = c == 27;
        return;
    }
}

void term_input(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) byte(buf[i]);
}

void term_init(int c, int r)
{
    cols = c > TERM_MAX_COLS ? TERM_MAX_COLS : c;
    rows = r > TERM_MAX_ROWS ? TERM_MAX_ROWS : r;
    reset();
    memset(shown, 0xff, sizeof shown);            /* forces a full first draw */
}

void term_render(void)
{
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            cell_t cl = G[r][c];
            if (cursor_on && r == cy && c == cx) cl.attr |= ATTR_CURSOR;
            cell_t *s = &shown[r][c];
            if (s->ch == cl.ch && s->fg == cl.fg && s->bg == cl.bg && s->attr == cl.attr) continue;
            *s = cl;
            fb_draw_cell(c, r, cl.ch, cl.fg, cl.bg, cl.attr);
        }
    }
}
