# GNU/seL4 — plan de trabajo

Estado: `[x]` hecho · `[~]` en curso · `[ ]` pendiente

## Hecho

- [x] Entorno: Microkit SDK 2.3.1, compilación en Docker (`dev.sh`, LLVM 18), QEMU+KVM en el host
- [x] Capa POSIX: musl 1.2.6 portada (syscalls → IPC de seL4), runtime, `sel4-cc`
- [x] `posix_server`: procesos en ranuras (exec, fork, wait, señales, grupos), tty, VFS, tuberías,
      `poll`/`select`, reloj (RTC + TSC) y temporizador
- [x] `fs_ext2`: ext2 propio sobre sDDF, verificado con `e2fsck`
- [x] dash 0.5.13.5 (`/bin/sh`); zsh como shell de sesión (según /etc/passwd); `/sbin/init` + `/etc/rc`
- [x] VM Linux con Docker (libvmm) en el mismo seL4; disco de 2 particiones; red para la VM
- [x] Linux transparente tipo WSL: `docker`, `linux comando`, pty interactiva
- [x] Archivos compartidos con la VM: `/mnt/sel4` por 9P (`9pd`)
- [x] Limpieza del repositorio (docker-vm viejo, demos, respaldos, builds de ejemplos: ~1.9 GB)
- [x] **GNU coreutils 9.12**: 101 programas en `/bin` (binario multi-llamada), sin headers de Linux;
      `uname -o` = GNU/seL4. Agregado al runtime: statx, utimensat, preadv/pwritev, sysinfo,
      prctl, sched_getaffinity, fadvise
- [x] `nice`/`renice` (getpriority/setpriority) y un `clear` provisional

## En orden

1. [x] Limpieza del repositorio
2. [x] GNU coreutils (reemplazó a `utiles`)
3. [x] **zsh y vim**: ncurses 6.6 (clear, tput, reset… + terminfo), zsh 5.9.2 (con /etc/zshrc),
       vim 9.2 (runtime reducido). Agregado: asignador mmap con munmap real, setitimer/alarm
       (SIGALRM), chown/fchown/lchown/fchownat
   - [x] RAM en el host 4.3 → 2.2 GB: seL4 pone a cero cada bloque untyped entero al usarlo, así
         que todo debe caber en los 2 GiB bajos. Ranuras: imagen 8 MiB, heap 4, mmap 64; VM de
         768 MiB con initramfs recortado (337 → 252 MiB: sin módulos inútiles ni `ctr`)
4. [ ] **Gestor de memoria dinámico en espacio de usuario**
   - [x] Microkit 2.3.1 compilado desde el código (`make microkit-tool`, parche en
         `proyectos/gnu-sel4/microkit/`): `<cap_untyped>`, `<cap_cnode>`, `<cap_asid_pool>`,
         `<cap_sched_pool>` y `<cap_endpoint>` en `<cspace>`
   - [x] Procesos a demanda (`server/mem.c`): posix_server crea TCB, espacio de direcciones,
         CNode y notificación con su untyped; páginas en el fallo de página; exit/exec/munmap
         las devuelven. Sin PDs de ranura en meta.py. Memoria de procesos en reposo: 924 MiB
         reservados → ~4 MiB usados; hasta 32 procesos; `free` provisional
   - [ ] Copy-on-write en fork (hoy copia todas las páginas)
   - [ ] Más de 32 procesos (máscaras de 64 bits en vfs/tty/bridge o listas de espera)
   - [ ] Páginas grandes (2 MiB) para regiones grandes; liberar tablas de páginas sin uso
5. [~] **Red nativa con la pila de FreeBSD**
   - [x] Componente `netstack` (`net/`): la pila TCP/IP de FreeBSD de F-Stack 1.25
         (`third_party/f-stack`, solo `lib/` y `freebsd/`, sin DPDK) sobre la red de sDDF, con su
         propio copiador; musl dentro del PD para la libc. TCP, UDP, IPv4/IPv6, ARP, rutas
   - [x] Sockets POSIX: posix_server (`OF_SOCKET` en vfs.c) llama a netstack por PPC
         (`include/sel4posix/net_protocol.h`); bloqueo con POSIX_RETRY y avisos por kqueue.
         socket, bind, connect, listen, accept(4), send/recv(from/msg), shutdown, sockopts,
         get{sock,peer}name, poll/select, FIONBIO/FIONREAD, O_NONBLOCK. DNS con /etc/resolv.conf
   - [x] `nettest` (DNS + HTTP a internet; `nettest servir` desde el host: puerto 8090)
   - [x] DHCP (`net/dhcp.c`): cliente a nivel de tramas, antes de la pila; la interfaz de FreeBSD
         se crea con la concesión (INIT-REBOOT con la IP preferida, DISCOVER, renovación en T1).
         posix_server reescribe /etc/resolv.conf con los DNS recibidos; `/proc/net/dhcp`.
         MAC fijas (antes cambiaban en cada compilación). `make NETSTACK_IP_PREFERIDA=` = sin preferencia
   - [ ] Configurar la red a mano (`ip`/`ifconfig`/`route`) y IP fija opcional en /etc
   - [ ] Loopback (127.0.0.1) y socketpair/AF_UNIX
   - [ ] Herramientas: curl/wget, ssh, nc; servicios (sshd)
   - [ ] Más de 64 KiB por llamada y menos copias (hoy: proceso -> servidor -> netstack)
6. [ ] **Robustez** (siguiente: protege todo lo demás)
   - [~] Control de versiones: repositorio git (`.gitignore`, `DEPENDENCIAS.md`); falta el push a
         github.com:jgmr2/gnu-sel4 (registrar la llave SSH) y un script que baje las dependencias
   - [x] `make test` (`tools/prueba.py`): copia la partición del sistema, crea /etc/prueba, arranca
         QEMU sin pantalla; /etc/pruebas.sh corre proctest, fase1test, archivos, coreutils, vim,
         procps y nettest y escribe en /dev/kmsg (nuevo: al puerto serie). 7 pruebas en ~11 s.
         `make test SIN_RED=1` sin internet
   - [ ] Tuberías con nombre (`mkfifo`): FIFO en ext2 que el servidor trata como tubería
         (las usa s6 para controlar los servicios)
   - [ ] s6 + s6-rc como supervisor (skarnet, ISC; mantenido, en C; se eligió sobre runit, que
         no se mantiene desde 2014 y no tiene dependencias): skalibs + s6 + s6-rc (+ execline).
         `s6-svscan` como PID 1 o lanzado por `/sbin/init`; servicios: 9pd, login + zsh; después
         dhcp, sshd, sway. s6-rc: dependencias ("sshd después de la red") y tareas de una vez
         (fsck, hora, red). `s6-svc`/`s6-rc` para ver, parar y reiniciar. skalibs sin signalfd
         hasta la Fase 1b (usa su método alternativo)
   - [ ] `poweroff`/`reboot` reales: parar servicios, `sync`, apagar la VM, apagar la máquina (ACPI)
   - [ ] fsck de ext2 al arrancar si el disco quedó sucio (bandera de estado de ext2)
7. [~] **Escritorio gráfico: sway + Wayland**
   - [x] vim con resaltado de sintaxis (faltaban syncolor.vim y colors/lists en el runtime)
   - [x] Fase 0: consola gráfica `fbcon` (`console/`): bochs-display de QEMU a 1280x800 (160x50),
         teclado PS/2 (i8042, US), emulador de terminal xterm-256color propio (UTF-8, 256 colores
         y 24 bits, pantalla alternativa, regiones, DEC graphics, respuestas DA/CPR), fuente
         Spleen 8x16. Es el "driver serial" de sDDF: nada más cambió. Salida 17x más rápida que
         el puerto serie (seq 20000: 4.25 s -> 0.24 s). `make CONSOLA=serie` vuelve al UART
   - [ ] Fase 0a: framebuffer GOP de UEFI (laptop); escala x2 de la fuente para pantallas HiDPI;
         historial con Shift+RePág
   - [ ] Fase 0b: ratón PS/2; teclado latinoamericano; consolas virtuales
   - [x] Fase 1: lo que pide Wayland a POSIX (`fase1test`, todo pasa y sin fugas):
         epoll (en el runtime, sobre poll), eventfd y timerfd (servidor); sockets AF_UNIX STREAM
         con bind/listen/accept/connect, socketpair, SCM_RIGHTS y SO_PEERCRED (servidor);
         memfd_create + ftruncate + mmap MAP_SHARED entre procesos (mem.c: copias de la
         capacidad de cada página; fork las comparte); mmap de archivos (copia privada); flock
   - [ ] Fase 1b: signalfd, shm_open (/dev/shm), AF_UNIX DGRAM/SEQPACKET, epoll anidado en poll
   - [ ] Fase 2: bibliotecas: libwayland, wayland-protocols, libffi, libxkbcommon (+ mapas),
         pixman, json-c, pcre2; cairo + pango (glib, harfbuzz, freetype, fontconfig).
         meson/ninja en la imagen de compilación
   - [ ] Fase 3: wlroots con un backend propio para seL4 (framebuffer + entrada; render pixman)
         y sway sin cambios
   - [ ] Fase 4: foot como terminal nativo; ventanas de la VM Linux con waypipe (como WSLg);
         bordes de color por origen (nativo / VM), estilo Qubes
8. [ ] **Integración con la VM Linux** (la VM corre Linux a velocidad nativa con VT-x; lo lento es
       el puente actual: colas tipo puerto serie con copias byte por byte)
   - [ ] vsock (AF_VSOCK): sockets POSIX entre procesos nativos y de la VM (virtio-vsock en libvmm,
         AF_VSOCK en posix_server); `docker`/`linux` encima de vsock en vez del puente actual
   - [ ] Archivos compartidos rápidos: 9P o virtio-fs sobre colas en memoria compartida
   - [ ] Memoria dinámica para la VM: la VM ve mucha RAM pero cada página se crea en su primer uso
         (fallo de EPT en libvmm -> página del pool) y vuelve al pool cuando Linux la libera
         (virtio-balloon con "free page reporting"). Ventana de la RAM de la VM en el VMM también
         a demanda. Tope por VM y reserva para lo nativo (que un contenedor no se coma el escritorio)
   - [ ] Servidor de memoria: sacar mem.c de posix_server a un componente que reparta páginas a
         posix_server y al VMM (un solo pool); Microkit: capacidad de la EPT de la VM
   - [ ] El sistema de la VM en disco en vez de initramfs en RAM (hoy 252 MB fijos)
9. [ ] **Journal en fs_ext2** (camino a ext3/ext4)
10. [ ] **Licencias**: elegir licencia del código propio, encabezados SPDX, `THIRD_PARTY.md`
11. [ ] **Arranque en hardware real (Dell Latitude 7390 2-in-1, UEFI)**
   - [ ] ISO live: GRUB (multiboot2) carga seL4 + sistema + disco ext2 en RAM
   - [ ] Framebuffer GOP (dirección y formato desde multiboot2) para la consola gráfica; disco en RAM
   - [ ] Disco persistente con el driver NVMe de sDDF
   - [ ] Wi-Fi (Intel 8265), USB, audio: passthrough (VT-d) a la VM Linux como "VM de drivers"
   - [ ] Touchpad/touchscreen (I²C-HID)
   - [ ] Red nativa por la VM de drivers: virtio-net entre netstack y la VM (el puente actual,
         tipo puerto serie, no sirve para datos)
   - [ ] Alternativa a la VM de drivers para Wi-Fi: el driver de Linux dentro de un componente
         (LKL o estilo DDE de Genode), sin VM
   - [ ] Multinúcleo (hoy seL4 corre en un solo núcleo; la laptop tiene 4)
   - [ ] Compilación release (kernel y sistema sin depuración)

## Huecos críticos (revisión del 2026-10-09)

Lo que más pesa para usarlo a diario, además de lo de arriba:

- [ ] **Hilos (pthreads)**: varios TCB de seL4 en un espacio de direcciones. Bloquea glib (pango,
      sway), navegadores y casi cualquier programa grande -> antes de cairo/pango (Fase 2)
- [ ] **Más de 32 procesos** y tablas del servidor más grandes (hoy: 128 archivos abiertos,
      32 tuberías, 64 sockets): un escritorio las rebasa
- [ ] **posix_server es un punto único de falla**: si cae, cae todo lo nativo; separarlo en
      servidores (procesos/memoria, archivos, red, tty) a largo plazo
- [ ] **Usuarios y permisos**: hoy todo es root y no se comprueban permisos; login real
- [ ] **Instalar software sin recompilar la imagen** (paquetes o actualizaciones); partición ext2
      más grande (64 MB ya no alcanza para la Fase 2)
- [ ] Herramientas de uso diario: grep, sed, findutils, diffutils, less, tar, gzip

## Pendientes conocidos (sin fecha)

- [ ] Señales asíncronas a procesos que no hacen syscalls (bucles puros); `Ctrl-Z` / parar procesos
- [ ] `Ctrl-C` a mitad del prompt de dash se procesa con la línea siguiente
- [ ] Triple propio (`x86_64-sel4-musl`) en vez de `x86_64-linux-musl` (hoy los programas ven `__linux__`)
- [ ] `mmap` compartido de archivos para escribir (hoy: copia privada, o compartido solo lectura)
- [ ] Dueño del proceso (uid/gid) al crear archivos (hoy siempre root); enlaces duros en fs_ext2
- [x] Tamaño real de la terminal: `resize` (pregunta a la terminal con CSI 6n) al iniciar sesión,
      TIOCSWINSZ + SIGWINCH. Si cambias el tamaño de la ventana: `resize`
- [ ] `timer_create` (temporizadores POSIX) e hilos (pthreads)
- [x] `/proc` virtual (`server/procfs.c`) + procps-ng 4.0.7: ps, top, free, uptime, pgrep, pkill,
      pidof, watch, vmstat, pwdx, tload. CPU por proceso con los scheduling contexts de seL4
- [ ] /proc: loadavg real, /proc/<pid>/maps y fd, tty con nombre (hoy "?")
- [ ] Que `nice` cambie la prioridad real en seL4 (hoy solo se guarda el valor)
- [ ] Port a ARM64 / Raspberry Pi 4 (falta driver SD en sDDF)
