/*
 * qrlog: shows a text file as a series of QR codes on the terminal, to get logs out of a
 * machine with no network and no writable storage (the live system on real hardware): take a
 * photo of each code with a phone and decode it there.
 *
 *   qrlog FILE        one code per page; Enter shows the next one, q quits
 *
 * Each code holds "gnu-sel4 FILE i/n" on its first line and then up to 1250 bytes of the file,
 * cut at line ends. Two modules per character cell, top and bottom ("▀", "▄", "█"), black on
 * white: on the 240x67 console of a 1920x1080 screen a module is 8x8 pixels.
 * QR encoding: Project Nayuki's qrcodegen (MIT, third_party/qrcodegen).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "qrcodegen.h"

#define CHUNK 1250                  /* bytes of the file per code: version 26 at low ECC holds ~1360 */

static void show(const uint8_t *qr, const char *title)
{
    int size = qrcodegen_getSize(qr), q = 4;          /* quiet zone of 4 modules */
    printf("\033[2J\033[H%s\n", title);
    for (int y = -q; y < size + q; y += 2) {
        printf("\033[30;107m");                         /* black on bright white */
        for (int x = -q; x < size + q; x++) {
            int top = y >= 0 && y < size && x >= 0 && x < size && qrcodegen_getModule(qr, x, y);
            int bot = y + 1 >= 0 && y + 1 < size && x >= 0 && x < size && qrcodegen_getModule(qr, x, y + 1);
            fputs(top && bot ? "█" : top ? "▀" : bot ? "▄" : " ", stdout);
        }
        printf("\033[0m\n");
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: qrlog FILE\n"); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    static char text[1 << 20];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;

    /* Pages cut at line ends */
    static size_t starts[1024];
    int pages = 0;
    for (size_t at = 0; at < len && pages < 1024; ) {
        size_t end = at + CHUNK < len ? at + CHUNK : len;
        if (end < len) {
            size_t nl = end;
            while (nl > at && text[nl - 1] != '\n') nl--;
            if (nl > at) end = nl;
        }
        starts[pages++] = at;
        at = end;
    }
    if (!pages) { fprintf(stderr, "qrlog: %s is empty\n", argv[1]); return 1; }
    starts[pages] = len;

    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    static char payload[CHUNK + 256];
    for (int p = 0; p < pages; p++) {
        int n = snprintf(payload, sizeof payload, "gnu-sel4 %s %d/%d\n", argv[1], p + 1, pages);
        size_t take = starts[p + 1] - starts[p];
        memcpy(payload + n, text + starts[p], take);
        payload[n + take] = 0;
        if (!qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW, 1, 26, qrcodegen_Mask_AUTO, true)) {
            fprintf(stderr, "qrlog: page %d does not fit in a code\n", p + 1);
            return 1;
        }
        char title[160];
        snprintf(title, sizeof title, "%s: code %d of %d  -  Enter: next, q: quit", argv[1], p + 1, pages);
        show(qr, title);
        fflush(stdout);
        char line[16];
        if (!fgets(line, sizeof line, stdin) || line[0] == 'q') break;
    }
    printf("\033[2J\033[H");
    return 0;
}
