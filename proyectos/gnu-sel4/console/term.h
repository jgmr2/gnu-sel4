/* Emulador de terminal de la consola gráfica (subconjunto de xterm: TERM=xterm-256color) */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_COLS 256
#define TERM_MAX_ROWS 128

#define ATTR_BOLD      1
#define ATTR_UNDERLINE 2
#define ATTR_REVERSE   4
#define ATTR_CURSOR    8           /* solo al dibujar: la celda lleva el cursor */

void term_init(int cols, int rows);
void term_input(const uint8_t *buf, size_t n);    /* salida de los programas */
void term_render(void);                           /* dibuja las celdas que cambiaron */
bool term_app_cursor(void);                       /* modo DECCKM: flechas como ESC O x */

/* Las da fbcon.c */
void term_reply(const char *s, size_t n);         /* respuestas (DA, CPR): como si se teclearan */
void fb_draw_cell(int col, int row, uint32_t ch, uint32_t fg, uint32_t bg, int attrs);
