/*
 * posix_server's process server.
 *
 * A process runs in a "slot": a thread with its own address space that mem.c
 * creates on demand, with the regions of include/sel4posix/procmem.h.
 * posix_server holds its TCB (it can stop it and restart it with other
 * registers) and sees its memory through a window, so it can load programs
 * and copy processes:
 *
 *   exec  reads a static ELF from ext2, copies it into the slot's image,
 *         builds the stack like Linux (argc, argv, envp, auxv) and jumps to e_entry
 *   fork  copies the process's pages into a free slot and starts the child
 *         at the point the runtime gives (which longjmps back into fork)
 *   exit  stops the slot, gives its memory back and leaves a zombie for wait()
 *
 * Signals are decided here (ignore, terminate or mark as pending); the
 * handlers run in the process's runtime (runtime/syscall.c).
 */
#include <os/sddf.h>
#include <string.h>
#include "server.h"
#include "syscall_nr.h"

/* ELF64 (the server has no libc nor <elf.h>) */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;
#define ELFMAG      "\177ELF"
#define SELFMAG     4
#define EI_CLASS    4
#define ELFCLASS64  2
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
/* Auxiliary vector */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_SECURE   23
#define AT_RANDOM   25

#define WNOHANG    1
#define S_IFMT     0170000
#define S_IFREG    0100000

struct proc procs[NPROC];
struct proc *cur;
static struct proc *slot_owner[PROC_SLOTS];
static int next_pid = 1;

struct proc *proc_by_slot(int slot)
{
    return slot >= 0 && slot < PROC_SLOTS ? slot_owner[slot] : NULL;
}

struct proc *proc_by_pid(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid) return &procs[i];
    return NULL;
}

void proc_wake(struct proc *p)
{
    if (p && p->state == P_RUN && p->slot >= 0) slot_signal(p->slot);
}

void proc_wake_mask(uint32_t mask)
{
    for (int i = 0; mask && i < NPROC; i++, mask >>= 1)
        if (mask & 1) proc_wake(&procs[i]);
}

static struct proc *proc_alloc(void)
{
    for (int i = 0; i < NPROC; i++) {
        if (procs[i].state == P_FREE) {
            struct proc *p = &procs[i];
            memset(p, 0, sizeof(*p));
            p->state = P_RUN;
            p->pid = next_pid++;
            p->slot = -1;
            p->start_ns = timer_now_ns();
            return p;
        }
    }
    return NULL;
}

static int slot_alloc(struct proc *p)
{
    for (int s = 0; s < PROC_SLOTS; s++) {
        if (!slot_owner[s] && slot_make(s)) {
            slot_owner[s] = p;
            p->slot = s;
            mem_slot_consumed_us(s);                     /* the CPU used before belonged to another process */
            return s;
        }
    }
    return -1;
}

static void set_name(struct proc *p, const char *path)
{
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
    size_t n = strlen(base);
    if (n >= sizeof(p->name)) n = sizeof(p->name) - 1;
    memcpy(p->name, base, n);
    p->name[n] = 0;
}

static long start_thread(int slot, seL4_UserContext *ctx)
{
    seL4_Error e = seL4_TCB_WriteRegisters(slot_tcb(slot), seL4_True, 0,
                                           sizeof(*ctx) / sizeof(seL4_Word), ctx);
    return e == seL4_NoError ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/* exec: ELF loader                                                   */
/* ------------------------------------------------------------------ */

#define MAX_PHDRS 16

/* Copies data onto the process's stack (through the server's window) and returns
 * its address inside the process. Out of memory, the stack is left half-built: *ok = false */
static uintptr_t push_bytes(int slot, uintptr_t *sp, const void *data, size_t n, bool *ok)
{
    *sp -= n;
    if (!mem_ensure(slot, *sp, n)) *ok = false;
    else memcpy((void *)proc_win(slot, *sp), data, n);
    return *sp;
}

/* Loads and starts a program in p's slot. argv and envp come as consecutive
 * strings (argc strings and then envc strings) in args. */
static long load_program(struct proc *p, const char *path, const char *args, uint32_t argc, uint32_t envc)
{
    char abs[FS_PATH_MAX];
    long r = vfs_normalize(p, path, abs);
    if (r) return r;
    uint32_t mode;
    uint64_t size;
    long ino = vfs_lookup(abs, true, &mode, &size);
    if (ino < 0) return ino;
    if ((mode & S_IFMT) != S_IFREG) return -EACCES;
    if (!(mode & 0111)) return -EACCES;

    Elf64_Ehdr eh;
    if (vfs_read_file(ino, 0, &eh, sizeof eh) != sizeof eh) return -ENOEXEC;

    /* "#!interpreter [arg]" script: run the interpreter with the script's path */
    if (((char *)&eh)[0] == '#' && ((char *)&eh)[1] == '!') {
        static char line[128], newargs[POSIX_BUF_SIZE];
        long n = vfs_read_file(ino, 2, line, sizeof line - 1);
        if (n <= 0) return -ENOEXEC;
        line[n] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *interp = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        char *iarg = NULL;
        if (*s == ' ' || *s == '\t') {
            *s++ = 0;
            while (*s == ' ' || *s == '\t') s++;
            if (*s && *s != '\n') iarg = s;
            while (*s && *s != '\n') s++;
        }
        *s = 0;
        if (!*interp) return -ENOEXEC;
        /* new argv: interpreter [arg] path argv[1..] */
        char *o = newargs, *end = newargs + sizeof newargs;
        size_t l = strlen(interp) + 1;
        memcpy(o, interp, l); o += l;
        uint32_t nargc = 1;
        if (iarg) { l = strlen(iarg) + 1; memcpy(o, iarg, l); o += l; nargc++; }
        l = strlen(abs) + 1; memcpy(o, abs, l); o += l; nargc++;
        const char *a = args;
        a += strlen(a) + 1;                                      /* skip the old argv[0] */
        for (uint32_t i = 1; i < argc + envc; i++) {
            l = strlen(a) + 1;
            if (o + l > end) return -E2BIG;
            memcpy(o, a, l); o += l; a += l;
        }
        static char interp_copy[FS_PATH_MAX];
        memcpy(interp_copy, interp, strlen(interp) + 1);
        return load_program(p, interp_copy, newargs, nargc + (argc ? argc - 1 : 0), envc);
    }

    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_machine != EM_X86_64 || eh.e_type != ET_EXEC || eh.e_phnum > MAX_PHDRS ||
        eh.e_phentsize != sizeof(Elf64_Phdr))
        return -ENOEXEC;
    Elf64_Phdr ph[MAX_PHDRS];
    if (vfs_read_file(ino, eh.e_phoff, ph, eh.e_phnum * sizeof(Elf64_Phdr)) != (long)(eh.e_phnum * sizeof(Elf64_Phdr)))
        return -ENOEXEC;
    uint64_t img_end = PROC_IMG_BASE, phdr_vaddr = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < PROC_IMG_BASE || ph[i].p_vaddr + ph[i].p_memsz > PROC_IMG_BASE + PROC_IMG_SIZE ||
            ph[i].p_filesz > ph[i].p_memsz)
            return -ENOEXEC;
        if (ph[i].p_vaddr + ph[i].p_memsz > img_end) img_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (eh.e_phoff >= ph[i].p_offset && eh.e_phoff < ph[i].p_offset + ph[i].p_filesz)
            phdr_vaddr = ph[i].p_vaddr + (eh.e_phoff - ph[i].p_offset);
    }

    /* The arguments are in the slot's buffer, which loading does not touch;
     * from here on there is no way back: the previous program's memory is
     * returned and the new one's is created as its segments are loaded */
    int slot = p->slot;
    mem_free_all(slot);
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (!mem_ensure(slot, ph[i].p_vaddr, ph[i].p_filesz)) {
            tty_puts("[posix] exec: out of memory\n");
            proc_exit(p, SIGKILL);
            return -ENOMEM;
        }
        uintptr_t dst = proc_win(slot, ph[i].p_vaddr);
        if (vfs_read_file(ino, ph[i].p_offset, (void *)dst, ph[i].p_filesz) != (long)ph[i].p_filesz) {
            proc_exit(p, SIGSEGV);
            return -EIO;
        }
        /* The rest up to p_memsz (.bss) shows up zeroed when touched; only the
         * tail of the last page holding file data has to be cleared */
        uintptr_t tail = (ph[i].p_vaddr + ph[i].p_filesz + 0xfff) & ~0xfffUL;
        uintptr_t mem_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (tail > mem_end) tail = mem_end;
        if (tail > ph[i].p_vaddr + ph[i].p_filesz)
            memset((void *)(dst + ph[i].p_filesz), 0, tail - (ph[i].p_vaddr + ph[i].p_filesz));
    }
    p->img_end = img_end;

    /* Initial stack, as Linux lays it out */
    uintptr_t sp = PROC_STACK_BASE + PROC_STACK_SIZE;
    bool ok = true;
    static uintptr_t ptrs[2048];
    uint32_t total = argc + envc;
    if (total + 2 > 2048) return -E2BIG;
    const char *a = args;
    for (uint32_t i = 0; i < total; i++) {
        size_t l = strlen(a) + 1;
        ptrs[i] = push_bytes(slot, &sp, a, l, &ok);
        a += l;
    }
    static const unsigned char rnd[16] = "GNU/seL4-random";
    uintptr_t at_random = push_bytes(slot, &sp, rnd, 16, &ok);
    sp &= ~15UL;
    uint64_t auxv[] = {
        AT_PHDR, phdr_vaddr, AT_PHENT, sizeof(Elf64_Phdr), AT_PHNUM, eh.e_phnum,
        AT_PAGESZ, 4096, AT_ENTRY, eh.e_entry, AT_RANDOM, at_random,
        AT_UID, 0, AT_EUID, 0, AT_GID, 0, AT_EGID, 0, AT_SECURE, 0,
        AT_SEL4_IPCBUF, PROC_IPCBUF,
        AT_SEL4_EPOCH, clock_epoch, AT_SEL4_TSC0, clock_tsc0, AT_SEL4_TSCHZ, clock_tsc_hz,
        AT_NULL, 0,
    };
    /* argc + argv + NULL + envp + NULL + auxv, ending 16-byte aligned */
    size_t words = 1 + argc + 1 + envc + 1 + sizeof(auxv) / 8;
    if (words & 1) sp -= 8;
    push_bytes(slot, &sp, auxv, sizeof auxv, &ok);
    uint64_t zero = 0;
    push_bytes(slot, &sp, &zero, 8, &ok);
    for (uint32_t i = envc; i > 0; i--) push_bytes(slot, &sp, &ptrs[argc + i - 1], 8, &ok);
    push_bytes(slot, &sp, &zero, 8, &ok);
    for (uint32_t i = argc; i > 0; i--) push_bytes(slot, &sp, &ptrs[i - 1], 8, &ok);
    uint64_t argc64 = argc;
    push_bytes(slot, &sp, &argc64, 8, &ok);
    if (!ok) {
        tty_puts("[posix] exec: out of memory\n");
        proc_exit(p, SIGKILL);
        return -ENOMEM;
    }

    /* For /proc: program and arguments */
    size_t el = strlen(abs);
    if (el >= sizeof p->exe) el = sizeof p->exe - 1;
    memcpy(p->exe, abs, el);
    p->exe[el] = 0;
    const char *ca = args;
    p->cmdline_len = 0;
    for (uint32_t i = 0; i < argc; i++) {
        size_t l = strlen(ca) + 1;
        if (p->cmdline_len + l > sizeof p->cmdline) break;
        memcpy(p->cmdline + p->cmdline_len, ca, l);
        p->cmdline_len += l;
        ca += l;
    }

    vfs_close_cloexec(p);
    p->sig_caught = 0;                 /* handlers do not survive exec */
    p->sig_pending &= p->sig_ignored ? ~p->sig_ignored : ~0ULL;
    set_name(p, abs);

    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rip = eh.e_entry;
    ctx.rsp = sp;
    return start_thread(slot, &ctx);
}

static long sys_execve(void)
{
    const char *path = (const char *)client_buf;
    const uint32_t *counts = (const uint32_t *)(client_buf + POSIX_BUF_PATH2);
    const char *args = (const char *)(client_buf + POSIX_BUF_PATH2 + 8);
    /* Check that the strings fit in the buffer */
    const char *a = args, *end = (const char *)(client_buf + POSIX_BUF_SIZE);
    for (uint32_t i = 0; i < counts[0] + counts[1]; i++) {
        while (a < end && *a) a++;
        if (a++ >= end) return -E2BIG;
    }
    ((char *)client_buf)[FS_PATH_MAX - 1] = 0;
    return load_program(cur, path, args, counts[0], counts[1]);
}

/* ------------------------------------------------------------------ */
/* fork                                                                */
/* ------------------------------------------------------------------ */

static long sys_fork(uintptr_t brk_cur, uintptr_t mmap_cur, uintptr_t sp, uintptr_t entry, uintptr_t fs_base)
{
    if (brk_cur < PROC_HEAP_BASE || brk_cur > PROC_HEAP_BASE + PROC_HEAP_SIZE ||
        mmap_cur < PROC_MMAP_BASE || mmap_cur > PROC_MMAP_BASE + PROC_MMAP_SIZE ||
        sp < PROC_STACK_BASE + 0x4000 || sp > PROC_STACK_BASE + PROC_STACK_SIZE ||
        entry < PROC_IMG_BASE || entry >= cur->img_end)
        return -EINVAL;
    struct proc *c = proc_alloc();
    if (!c) return -EAGAIN;
    if (slot_alloc(c) < 0) {
        c->state = P_FREE;
        return -EAGAIN;
    }
    c->ppid = cur->pid;
    c->pgid = cur->pgid;
    c->sid = cur->sid;
    c->img_end = cur->img_end;
    c->sig_caught = cur->sig_caught;
    c->sig_ignored = cur->sig_ignored;
    c->nice = cur->nice;
    memcpy(c->name, cur->name, sizeof c->name);
    memcpy(c->exe, cur->exe, sizeof c->exe);
    memcpy(c->cmdline, cur->cmdline, cur->cmdline_len);
    c->cmdline_len = cur->cmdline_len;
    vfs_fork(c, cur);

    /* Copy every page of the parent (the ones that exist: the others show up
     * zeroed in both when touched) */
    int ps = cur->slot, cs = c->slot;
    if (!mem_fork(cs, ps)) {
        tty_puts("[posix] fork: out of memory\n");
        proc_exit(c, SIGKILL);
        return -ENOMEM;
    }

    /* The child starts at entry (which longjmps to the fork point) with a
     * temporary stack inside the copied memory */
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rip = entry;
    ctx.rsp = ((sp - 0x2000) & ~15UL) - 8;
    ctx.fs_base = fs_base;
    if (start_thread(cs, &ctx)) {
        proc_exit(c, SIGKILL);
        return -EAGAIN;
    }
    return c->pid;
}

/* ------------------------------------------------------------------ */
/* exit and wait                                                      */
/* ------------------------------------------------------------------ */

static void free_proc(struct proc *p) { p->state = P_FREE; }

/* Terminates a process. status in wait() format: (code << 8) or signal */
void proc_exit(struct proc *p, int status)
{
    if (p->state != P_RUN) return;
    if (p->slot >= 0) {
        seL4_TCB_Suspend(slot_tcb(p->slot));
        procfs_account(p);
        mem_free_all(p->slot);
        slot_owner[p->slot] = NULL;
        p->slot = -1;
    }
    struct proc *saved = cur;
    cur = p;
    vfs_close_all(p);
    cur = saved;

    /* Children are handed to init; orphaned zombies are freed */
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state == P_FREE || q->ppid != p->pid || q == p) continue;
        if (q->state == P_ZOMBIE) free_proc(q);
        else { q->ppid = 1; q->orphan = true; }
    }

    p->state = P_ZOMBIE;
    p->status = status;
    p->wait_child = false;

    if (p->pid == 1) {
        tty_puts("[posix] init exited with code ");
        tty_putdec((status >> 8) & 0xff);
        tty_puts(": restarting it\n");
        free_proc(p);
        proc_start_init();
        return;
    }
    struct proc *parent = p->orphan ? NULL : proc_by_pid(p->ppid);
    if (!parent) {
        free_proc(p);
        return;
    }
    proc_signal(parent, SIGCHLD);
    if (parent->wait_child) proc_wake(parent);
}

static bool wait_match(struct proc *q, long pid)
{
    if (pid > 0) return q->pid == pid;
    if (pid == -1) return true;
    if (pid == 0) return q->pgid == cur->pgid;
    return q->pgid == -pid;
}

static long sys_wait4(long pid, long options)
{
    bool any = false;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state == P_FREE || q->ppid != cur->pid || q->orphan || !wait_match(q, pid)) continue;
        any = true;
        if (q->state == P_ZOMBIE) {
            long r = q->pid;
            microkit_mr_set(1, q->status);
            free_proc(q);
            cur->wait_child = false;
            return r;
        }
    }
    if (!any) return -ECHILD;
    if (options & WNOHANG) return 0;
    cur->wait_child = true;
    return POSIX_RETRY;
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */
/* ------------------------------------------------------------------ */

static bool default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT ||
           sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;   /* no stopping processes yet */
}

void proc_signal(struct proc *p, int sig)
{
    if (!p || p->state != P_RUN || sig <= 0 || sig > 64) return;
    uint64_t bit = SIGBIT(sig);
    if (sig != SIGKILL && (p->sig_ignored & bit)) return;
    if (sig != SIGKILL && (p->sig_caught & bit)) {
        p->sig_pending |= bit;
        proc_wake(p);                      /* if it was blocked, it retries and gets EINTR */
        return;
    }
    if (default_ignore(sig)) return;
    proc_exit(p, sig);                     /* default action: terminate */
}

void proc_signal_pgrp(int pgid, int sig)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state == P_RUN && procs[i].pgid == pgid) proc_signal(&procs[i], sig);
}

static long sys_kill(long pid, long sig)
{
    if (sig < 0 || sig > 64) return -EINVAL;
    int found = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_RUN) continue;
        bool match = pid > 0 ? q->pid == pid : pid == 0 ? q->pgid == cur->pgid :
                     pid == -1 ? q->pid != 1 : q->pgid == -pid;
        if (!match) continue;
        found++;
        proc_signal(q, sig);
    }
    return found ? 0 : -ESRCH;
}

/* getpriority/setpriority. which: 0 = process, 1 = group, 2 = user (everyone is root).
 * Like the Linux kernel, getpriority returns 20 - nice (musl converts it). */
static long sys_priority(bool set, long which, long who, long prio)
{
    if (which < 0 || which > 2) return -EINVAL;
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    int found = 0, best = 19;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_RUN) continue;
        bool match = which == 0 ? q->pid == (who ? who : cur->pid)
                   : which == 1 ? q->pgid == (who ? who : cur->pgid)
                   : who == 0;
        if (!match) continue;
        found++;
        if (set) q->nice = prio;
        else if (q->nice < best) best = q->nice;
    }
    if (!found) return -ESRCH;
    return set ? 0 : 20 - best;
}

/* setitimer/getitimer (ITIMER_REAL only; the CPU-time ones are accepted with no effect).
 * The runtime passes value and interval in ns; the previous state comes back in MR1/MR2. */
static long sys_itimer(bool set, long which, uint64_t value_ns, uint64_t interval_ns)
{
    if (which < 0 || which > 2) return -EINVAL;
    if (which != 0) {
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        return 0;
    }
    uint64_t now = timer_now_ns();
    microkit_mr_set(1, cur->itimer_next > now ? cur->itimer_next - now : 0);
    microkit_mr_set(2, cur->itimer_interval);
    if (set) {
        cur->itimer_next = value_ns ? now + value_ns : 0;
        cur->itimer_interval = interval_ns;
        timer_rearm();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Slot faults (they arrive through their fault endpoint)            */
/* ------------------------------------------------------------------ */

/* true: reply and the process carries on (the page now exists) */
bool proc_fault(int slot, long label)
{
    struct proc *p = proc_by_slot(slot);
    if (!p) return false;
    if (label == seL4_Fault_VMFault && mem_fault(slot, microkit_mr_get(seL4_VMFault_Addr))) return true;
    /* Processor exception: as in Linux, general protection -> SIGSEGV, division -> SIGFPE */
    int sig = SIGILL;
    if (label == seL4_Fault_VMFault) sig = SIGSEGV;
    else if (label == seL4_Fault_UserException) {
        long vec = microkit_mr_get(seL4_UserException_Number);
        sig = vec == 0 ? SIGFPE : vec == 6 ? SIGILL : SIGSEGV;
    }
    tty_puts("[posix] ");
    tty_puts(p->name);
    tty_puts(" (pid ");
    tty_putdec(p->pid);
    tty_puts(sig == SIGSEGV ? "): segmentation fault at 0x" : sig == SIGFPE ? "): arithmetic error at 0x"
                                                             : "): illegal instruction at 0x");
    char hex[17];
    uint64_t v = label == seL4_Fault_VMFault ? microkit_mr_get(seL4_VMFault_Addr) : microkit_mr_get(0);
    for (int i = 15; i >= 0; i--, v >>= 4) hex[i] = "0123456789abcdef"[v & 15];
    hex[16] = 0;
    tty_puts(hex);
    tty_puts("\n");
    proc_exit(p, sig);
    return false;                          /* no reply: the slot stays stopped */
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

bool proc_handles(long nr)
{
    switch (nr) {
    case __NR_fork: case __NR_vfork: case __NR_execve: case __NR_wait4: case __NR_exit: case __NR_exit_group:
    case __NR_getpid: case __NR_getppid: case __NR_kill:
    case __NR_setpgid: case __NR_getpgid: case __NR_getpgrp: case __NR_setsid: case __NR_getsid:
    case __NR_rt_sigaction: case __NR_rt_sigsuspend:
    case __NR_getpriority: case __NR_setpriority:
    case __NR_setitimer: case __NR_getitimer:
    case __NR_munmap: case __NR_sysinfo:
        return true;
    }
    return false;
}

long proc_syscall(long nr, long a1, long a2, long a3, long a4, long a5)
{
    struct proc *q;
    switch (nr) {
    case __NR_fork:
    case __NR_vfork:      return sys_fork(a1, a2, a3, a4, a5);
    case __NR_execve:     return sys_execve();
    case __NR_wait4:      return sys_wait4(a1, a3);
    case __NR_exit:
    case __NR_exit_group: proc_exit(cur, (a1 & 0xff) << 8); return 0;
    case __NR_getpid:     return cur->pid;
    case __NR_getppid:    return cur->orphan ? 1 : cur->ppid;
    case __NR_kill:       return sys_kill(a1, a2);
    case __NR_setpgid:
        q = a1 ? proc_by_pid(a1) : cur;
        if (!q || (q != cur && q->ppid != cur->pid)) return -ESRCH;
        if (a2 < 0) return -EINVAL;
        q->pgid = a2 ? a2 : q->pid;
        return 0;
    case __NR_getpgid:
        q = a1 ? proc_by_pid(a1) : cur;
        return q ? q->pgid : -ESRCH;
    case __NR_getpgrp:    return cur->pgid;
    case __NR_setsid:
        if (cur->pgid == cur->pid) return -EPERM;
        cur->sid = cur->pgid = cur->pid;
        return cur->sid;
    case __NR_getsid:
        q = a1 ? proc_by_pid(a1) : cur;
        return q ? q->sid : -ESRCH;
    case __NR_rt_sigaction: {
        uint64_t bit = SIGBIT(a1);
        cur->sig_caught &= ~bit;
        cur->sig_ignored &= ~bit;
        if (a2 == 1) { cur->sig_ignored |= bit; cur->sig_pending &= ~bit; }
        if (a2 == 2) cur->sig_caught |= bit;
        return 0;
    }
    case __NR_rt_sigsuspend:
        return cur->sig_pending ? -EINTR : POSIX_RETRY;
    case __NR_getpriority:
    case __NR_setpriority:
        return sys_priority(nr == __NR_setpriority, a1, a2, a3);
    case __NR_setitimer:
    case __NR_getitimer:
        return sys_itimer(nr == __NR_setitimer, a1, a2, a3);
    case __NR_sysinfo: {                   /* process memory (MR1, MR2) and live processes (MR3) */
        int n = 0;
        for (int i = 0; i < NPROC; i++) n += procs[i].state == P_RUN;
        microkit_mr_set(1, mem_total());
        microkit_mr_set(2, mem_free());
        microkit_mr_set(3, n);
        return 0;
    }
    case __NR_munmap:                      /* the runtime already freed the range: give the pages back */
        if ((uintptr_t)a1 >= PROC_HEAP_BASE && (uintptr_t)a1 + a2 <= PROC_MMAP_BASE + PROC_MMAP_SIZE)
            mem_free_range(cur->slot, a1, a2);
        return 0;
    }
    return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/* Initial process                                                    */
/* ------------------------------------------------------------------ */

void proc_init(void)
{
}

/* Starts init (pid 1): /sbin/init, or /bin/sh as the login shell */
void proc_start_init(void)
{
    struct proc *p = proc_alloc();
    if (!p || slot_alloc(p) < 0) {
        tty_puts("[posix] no free slot for init\n");
        return;
    }
    p->pid = 1;
    p->ppid = 0;
    p->pgid = p->sid = 1;
    vfs_proc_tty(p);
    tty_set_fg(1);

    static const char *const candidates[][2] = {
        { "/sbin/init", "init" }, { "/bin/sh", "-sh" },
    };
    static const char env[] = "PATH=/bin:/sbin:/usr/bin\0HOME=/root\0TERM=xterm-256color\0";
    static char args[256];
    for (unsigned i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        size_t l0 = strlen(candidates[i][1]) + 1;
        memcpy(args, candidates[i][1], l0);
        memcpy(args + l0, env, sizeof env);
        cur = p;
        long r = load_program(p, candidates[i][0], args, 1, 3);
        if (r == 0) return;
    }
    tty_puts("[posix] neither /sbin/init nor /bin/sh was found: the system is left without init\n");
    slot_owner[p->slot] = NULL;
    vfs_close_all(p);
    free_proc(p);
}
