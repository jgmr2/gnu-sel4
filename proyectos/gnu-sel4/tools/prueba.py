#!/usr/bin/env python3
"""make test: pruebas automáticas de GNU/seL4 en QEMU, sin pantalla.

Copia la partición del sistema de disk.img a un disco temporal (el disco de verdad no se
toca), crea /etc/prueba en la copia (/etc/rc corre entonces /etc/pruebas.sh), arranca QEMU
y lee del puerto serie las líneas "PRUEBA ...". Sale con 0 solo si todas pasan.

  uso: tools/prueba.py [--sin-red] [--tiempo SEGUNDOS]
"""
import argparse, fcntl, os, select, subprocess, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
PROY = os.path.dirname(AQUI)
DISCO = os.path.join(PROY, "disk.img")
BUILD = os.path.join(PROY, "build")
COPIA = os.path.join(BUILD, "prueba-disco.img")
MARCA = os.path.join(BUILD, "prueba-marca")
PARTE1 = (1 + 64) << 20                    # MBR + partición ext2 (EXT2_MB en system.mk)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sin-red", action="store_true", help="omitir las pruebas que necesitan internet")
    ap.add_argument("--tiempo", type=int, default=240, help="límite en segundos")
    a = ap.parse_args()

    for f in ("sel4_32.elf", "loader.img"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit("falta build/%s: compila con ../../dev.sh make" % f)
    with open(DISCO, "r+b") as d:            # no copiar un disco que QEMU está usando
        try:
            fcntl.lockf(d, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sys.exit("disk.img está en uso (¿QEMU abierto?): ciérralo para hacer las pruebas")
        fcntl.lockf(d, fcntl.LOCK_UN)
        with open(COPIA, "wb") as c:
            c.write(d.read(PARTE1))
            c.truncate(os.path.getsize(DISCO))   # el resto (partición de Docker) vacío, sin ocupar

    with open(MARCA, "w") as m:
        m.write("sin-red" if a.sin_red else "completa")
    rel = lambda p: os.path.relpath(p, os.path.dirname(os.path.dirname(PROY)))
    r = subprocess.run([os.path.join(PROY, "..", "..", "dev.sh"), "debugfs", "-w", "-R",
                        "write %s /etc/prueba" % rel(MARCA).replace(os.sep, "/"),
                        "%s?offset=1048576" % rel(COPIA).replace(os.sep, "/")],
                       cwd=os.path.dirname(os.path.dirname(PROY)), capture_output=True, text=True)
    if r.returncode:
        sys.exit("debugfs: " + r.stderr)

    qemu = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
            "-cpu", "host,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave,+vmx,+vme",
            "-kernel", os.path.join(BUILD, "sel4_32.elf"), "-initrd", os.path.join(BUILD, "loader.img"),
            "-m", "6G", "-display", "none", "-vga", "none", "-device", "bochs-display,addr=0x5",
            "-serial", "stdio", "-monitor", "none",
            "-drive", "file=%s,format=raw,if=none,id=hd" % COPIA, "-device", "virtio-blk-pci,drive=hd,addr=0x3.0",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,addr=0x4.0"]
    p = subprocess.Popen(qemu, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    os.set_blocking(p.stdout.fileno(), False)
    fin, resultados, buf, t0 = None, [], b"", time.time()
    try:
        while time.time() - t0 < a.tiempo and fin is None:
            if not select.select([p.stdout], [], [], 1)[0]:
                if p.poll() is not None:
                    break
                continue
            buf += p.stdout.read() or b""
            *lineas, buf = buf.split(b"\n")
            for l in lineas:
                t = l.decode("utf-8", "replace").rstrip("\r")
                if t.startswith("PRUEBA ") or t.startswith("    "):
                    print(t)
                    if t.startswith("PRUEBA "):
                        resultados.append(t)
                elif t.startswith("== PRUEBAS: FIN"):
                    fin = t
                elif "PANIC" in t or "faulting PD" in t:
                    print("  [serie] " + t)
    finally:
        p.kill()
        p.wait()
        os.remove(COPIA)
        os.remove(MARCA)

    if fin is None:
        print("NO TERMINÓ: las pruebas no llegaron al final en %d s" % a.tiempo)
        return 1
    malas = [r for r in resultados if "FALLA" in r]
    print("%s: %d pruebas, %d fallas (%.0f s)" % ("BIEN" if not malas else "MAL", len(resultados),
                                                   len(malas), time.time() - t0))
    return 1 if malas else 0

if __name__ == "__main__":
    sys.exit(main())
