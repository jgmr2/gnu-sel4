#!/usr/bin/env python3
"""Convierte una fuente BDF de 8 px de ancho (Spleen 8x16) en una tabla C para la consola.
   uso: bdf2c.py fuente.bdf salida.h
Genera font_cp[] (puntos de código, ordenados) y font_bits[][16] (una fila por byte)."""
import sys

src, out = sys.argv[1], sys.argv[2]
glyphs, cp, bits, height = {}, None, None, 16
for line in open(src, encoding="latin-1"):
    w = line.split()
    if not w:
        continue
    if w[0] == "FONTBOUNDINGBOX":
        assert w[1] == "8", "solo fuentes de 8 px de ancho"
        height = int(w[2])
    elif w[0] == "ENCODING":
        cp = int(w[1])
    elif w[0] == "BITMAP":
        bits = []
    elif w[0] == "ENDCHAR":
        if cp is not None and cp >= 0 and len(bits) == height:
            glyphs[cp] = bits
        cp, bits = None, None
    elif bits is not None:
        bits.append(int(w[0], 16) & 0xff)

with open(out, "w") as f:
    f.write("/* Generado por tools/bdf2c.py desde Spleen (BSD-2-Clause, (c) Frederic Cambus) */\n")
    f.write("#define FONT_W 8\n#define FONT_H %d\n#define FONT_N %d\n" % (height, len(glyphs)))
    f.write("static const uint32_t font_cp[FONT_N] = {\n")
    keys = sorted(glyphs)
    for i in range(0, len(keys), 12):
        f.write("    " + ", ".join("0x%04x" % k for k in keys[i:i + 12]) + ",\n")
    f.write("};\nstatic const uint8_t font_bits[FONT_N][FONT_H] = {\n")
    for k in keys:
        f.write("    {" + ", ".join("0x%02x" % b for b in glyphs[k]) + "},\n")
    f.write("};\n")
print("%s: %d glifos de 8x%d" % (out, len(glyphs), height))
