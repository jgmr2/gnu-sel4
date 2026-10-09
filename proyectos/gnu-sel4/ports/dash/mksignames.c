/*
 * Generador de signames.c para dash en GNU/seL4.
 *
 * Reemplaza al mksignames.c original (que viene de GNU bash y es GPL) con
 * una tabla fija de las señales de Linux x86_64, que es la numeración que
 * usa musl. Así dash queda enteramente bajo su licencia BSD.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>

#define NSIG_ 65   /* _NSIG de musl */

static const char *const names[32] = {
    "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
};

int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "signames.c", "w");   /* como el original */
    if (!out) return 1;
    fprintf(out, "/* Generado por mksignames (GNU/seL4). No editar. */\n\n");
    fprintf(out, "#include <signal.h>\n\n");
    fprintf(out, "const char *const signal_names[NSIG + 1] = {\n");
    for (int i = 0; i < NSIG_; i++) {
        if (i < 32) fprintf(out, "    \"%s\",\n", names[i]);
        else fprintf(out, "    \"%d\",\n", i);           /* señales de tiempo real: por número */
    }
    fprintf(out, "    (char *)0x0\n};\n");
    return fclose(out);
}
