/*
 * resize: averigua el tamaño real de la terminal y se lo dice al sistema (TIOCSWINSZ).
 * Por el puerto serie no llega el tamaño de la ventana: se lleva el cursor a la
 * esquina inferior derecha y se le pregunta a la terminal dónde quedó (CSI 6n).
 * Se ejecuta al iniciar sesión; si cambias el tamaño de la ventana, vuelve a ejecutarlo.
 *   resize       fija el tamaño e imprime COLUMNS/LINES como el resize de xterm
 *   resize -s    fija el tamaño sin imprimir nada
 */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    int quiet = argc > 1 && !strcmp(argv[1], "-s");
    struct termios old, raw;
    if (!isatty(0) || tcgetattr(0, &old)) return 1;
    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    tcsetattr(0, TCSAFLUSH, &raw);

    /* guardar cursor, ir a 999;999 (la terminal lo limita al borde), preguntar, restaurar */
    static const char q[] = "\0337\033[r\033[999;999H\033[6n\0338";
    write(1, q, sizeof q - 1);

    char buf[32];
    size_t n = 0;
    struct pollfd p = { .fd = 0, .events = POLLIN };
    while (n < sizeof buf - 1 && poll(&p, 1, 1000) == 1) {
        if (read(0, buf + n, 1) != 1) break;
        if (buf[n++] == 'R') break;
    }
    buf[n] = 0;
    tcsetattr(0, TCSAFLUSH, &old);

    int rows, cols;
    char *esc = strchr(buf, '\033');
    if (!esc || sscanf(esc, "\033[%d;%dR", &rows, &cols) != 2 || rows < 2 || cols < 2) {
        if (!quiet) fprintf(stderr, "resize: la terminal no respondió\n");
        return 1;
    }
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    ioctl(0, TIOCSWINSZ, &ws);
    if (!quiet) printf("COLUMNS=%d;\nLINES=%d;\nexport COLUMNS LINES;\n", cols, rows);
    return 0;
}
