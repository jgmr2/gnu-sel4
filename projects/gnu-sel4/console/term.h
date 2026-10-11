/* Terminal emulator of the graphical console (a subset of xterm: TERM=xterm-256color) */
/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 128

#define ATTR_BOLD      1
#define ATTR_UNDERLINE 2
#define ATTR_REVERSE   4
#define ATTR_CURSOR    8           /* only when drawing: the cell has the cursor */

void term_init(int cols, int rows);
void term_input(const uint8_t *buf, size_t n);    /* output from the programs */
void term_render(void);                           /* draws the cells that changed */
bool term_app_cursor(void);                       /* DECCKM mode: arrows as ESC O x */

/* Provided by fbcon.c */
void term_reply(const char *s, size_t n);         /* replies (DA, CPR): as if they were typed */
void fb_draw_cell(int col, int row, uint32_t ch, uint32_t fg, uint32_t bg, int attrs);
