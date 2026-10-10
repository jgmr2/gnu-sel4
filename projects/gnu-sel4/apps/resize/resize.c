/*
 * resize: finds out the real terminal size and tells the system (TIOCSWINSZ).
 * The window size does not arrive over the serial port: the cursor is moved to
 * the bottom-right corner and the terminal is asked where it ended up (CSI 6n).
 * It runs at login; if you resize the window, run it again.
 *   resize       sets the size and prints COLUMNS/LINES like xterm's resize
 *   resize -s    sets the size without printing anything
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

    /* save the cursor, go to 999;999 (the terminal clamps it to the edge), ask, restore */
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
        if (!quiet) fprintf(stderr, "resize: the terminal did not answer\n");
        return 1;
    }
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    ioctl(0, TIOCSWINSZ, &ws);
    if (!quiet) printf("COLUMNS=%d;\nLINES=%d;\nexport COLUMNS LINES;\n", cols, rows);
    return 0;
}
