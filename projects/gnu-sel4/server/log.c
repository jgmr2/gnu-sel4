/*
 * posix_server's log (the project's standard: TODO.md, "Logs"): JSON Lines in a ring, read at
 * /proc/log, one object per event: {"t":<ns since boot>,"c":"posix_server","l":<level>,"ev":<event>,
 * "s":<an optional string>,<the event's fields>}. Warnings and errors go to the console as well.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sddf/util/printf.h>
#include "server.h"

#define LOG_SIZE 0x10000
static char ring[LOG_SIZE];
static size_t pos;                     /* bytes written so far (the ring keeps the last ones) */

/* A string as JSON: quotes, backslashes and control characters escaped */
static size_t json_str(char *o, size_t max, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < max; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = c; }
        else if (c < 0x20) k += sddf_snprintf(o + k, max - k, "\\u%04x", c);
        else o[k++] = c;
    }
    return k;
}

void slog_line(const char *level, const char *ev, const char *str, const char *fields)
{
    char line[512];
    size_t max = sizeof line - 3, k = sddf_snprintf(line, max,
        "{\"t\":%" PRIu64 ",\"c\":\"posix_server\",\"l\":\"%s\",\"ev\":\"%s\"", timer_now_ns(), level, ev);
    if (str && k + 8 < max) {
        k += sddf_snprintf(line + k, max - k, ",\"s\":\"");
        k += json_str(line + k, max - k - 2, str);
        line[k++] = '"';
    }
    if (fields && *fields && k + 2 < max) {
        line[k++] = ',';
        for (const char *f = fields; *f && k < max; f++) line[k++] = *f;
    }
    line[k++] = '}';
    line[k++] = '\n';
    line[k] = 0;
    for (size_t i = 0; i < k; i++) ring[pos++ % LOG_SIZE] = line[i];
    if (level[0] == 'w' || level[0] == 'e') tty_puts(line);
}

/* /proc/log: the newest max bytes, from the start of a line */
size_t log_text(char *dst, size_t max)
{
    size_t have = pos < LOG_SIZE ? pos : LOG_SIZE, n = have < max ? have : max, from = pos - n;
    size_t skip = 0;
    if (n < pos) while (skip < n && ring[(from + skip) % LOG_SIZE] != '\n') skip++;
    if (skip < n && n < pos) skip++;
    for (size_t i = skip; i < n; i++) dst[i - skip] = ring[(from + i) % LOG_SIZE];
    return n - skip;
}
