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
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <string.h>
#include "server.h"
#include "arch.h"
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
#define WUNTRACED  2
#define WCONTINUED 8
#define S_IFMT     0170000
#define S_IFREG    0100000

struct proc procs[NPROC];
struct proc *cur;
bool reply_direct;
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

void proc_wake_mask(waitmask_t mask)
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

#ifndef __wasm__
static long start_thread(int slot, seL4_UserContext *ctx)
{
    seL4_Error e = seL4_TCB_WriteRegisters(slot_tcb(slot), seL4_True, 0,
                                           sizeof(*ctx) / sizeof(seL4_Word), ctx);
    return e == seL4_NoError ? 0 : -EIO;
}
#endif

/* ------------------------------------------------------------------ */
/* exec: ELF loader                                                   */
/* ------------------------------------------------------------------ */

#define MAX_PHDRS 16

/* Copies data onto the process's stack and returns
 * its address inside the process. Out of memory, the stack is left half-built: *ok = false */
static uintptr_t push_bytes(int slot, uintptr_t *sp, const void *data, size_t n, bool *ok)
{
    *sp -= n;
    if (!mem_copy_in(slot, *sp, data, n)) *ok = false;
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
    uint32_t fuid = 0, fgid = 0;
    vfs_owner(ino, &fuid, &fgid);
    if (!cred_may(p, mode, fuid, fgid, 1)) return -EACCES;      /* execute permission */

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

#ifdef __wasm__
    /* The browser port: a WebAssembly module, instantiated by the page in a worker of its own */
    if (memcmp(&eh, "\0asm", 4)) return -ENOEXEC;
    int slot = p->slot;
    mem_free_all(slot);
    p->img_ino = 0;
    p->nseg = 0;
    size_t args_len = 0;
    for (uint32_t i = 0; i < argc + envc; i++) args_len += strlen(args + args_len) + 1;
#else
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_machine != ARCH_ELF_MACHINE || eh.e_type != ET_EXEC || eh.e_phnum > MAX_PHDRS ||
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
    /* Up to 4 segments (static programs have 2-4): loaded page by page when touched */
    int nload = 0;
    for (int i = 0; i < eh.e_phnum; i++) nload += ph[i].p_type == PT_LOAD;
    p->img_ino = 0;
    p->nseg = 0;
    if (nload <= 4) {
        for (int i = 0; i < eh.e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) continue;
            p->seg[p->nseg].vaddr = ph[i].p_vaddr;
            p->seg[p->nseg].filesz = ph[i].p_filesz;
            p->seg[p->nseg].memsz = ph[i].p_memsz;
            p->seg[p->nseg].off = ph[i].p_offset;
            p->nseg++;
        }
        p->img_ino = ino;
    }
    for (int i = 0; i < eh.e_phnum && !p->img_ino; i++) {     /* otherwise, all of it now */
        if (ph[i].p_type != PT_LOAD) continue;
        if (!mem_ensure(slot, ph[i].p_vaddr, ph[i].p_filesz)) {
            tty_puts("[posix] exec: out of memory\n");
            proc_exit(p, SIGKILL);
            return -ENOMEM;
        }
        if (vfs_read_to_proc(ino, ph[i].p_offset, slot, ph[i].p_vaddr, ph[i].p_filesz) != (long)ph[i].p_filesz) {
            proc_exit(p, SIGSEGV);
            return -EIO;
        }
        /* The rest up to p_memsz (.bss) shows up zeroed when touched; only the
         * tail of the last page holding file data has to be cleared */
        uintptr_t tail = (ph[i].p_vaddr + ph[i].p_filesz + 0xfff) & ~0xfffUL;
        uintptr_t mem_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (tail > mem_end) tail = mem_end;
        if (tail > ph[i].p_vaddr + ph[i].p_filesz &&
            !mem_copy_in(slot, ph[i].p_vaddr + ph[i].p_filesz, NULL, tail - (ph[i].p_vaddr + ph[i].p_filesz))) {
            proc_exit(p, SIGKILL);
            return -ENOMEM;
        }
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
        AT_UID, p->uid, AT_EUID, p->euid, AT_GID, p->gid, AT_EGID, p->egid,
        AT_SECURE, p->euid != p->uid || p->egid != p->gid,       /* set-ID program: libc is careful */
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

#endif
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
    cred_exec(p, mode, fuid, fgid);   /* set-user-ID / set-group-ID programs */
    p->sig_caught = 0;                 /* handlers do not survive exec */
    p->async_ctl = p->async_entry = 0; /* the new program registers its own (POSIX_SYS_ASYNC) */
    p->sig_pending &= p->sig_ignored ? ~p->sig_ignored : ~0ULL;
    set_name(p, abs);

#ifdef __wasm__
    return mem_wasm_exec(slot, ino, size, args, args_len, argc, envc);
#else
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    arch_ctx_start(&ctx, eh.e_entry, sp, 0);
    return start_thread(slot, &ctx);
#endif
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
    long r = load_program(cur, path, args, counts[0], counts[1]);
    if (r < 0) slog("info", "exec-failed", path, "\"pid\":%d,\"err\":%ld", cur->pid, r);
    return r;
}

/* ------------------------------------------------------------------ */
/* fork                                                                */
/* ------------------------------------------------------------------ */

static long sys_fork(uintptr_t brk_cur, uintptr_t mmap_cur, uintptr_t sp, uintptr_t entry, uintptr_t fs_base)
{
#ifdef __wasm__
    (void)brk_cur; (void)mmap_cur; (void)sp; (void)entry; (void)fs_base;
    return -ENOSYS;                      /* the browser port: TODO item 24, experiment 2 */
#else
    if (brk_cur < PROC_HEAP_BASE || brk_cur > PROC_HEAP_BASE + PROC_HEAP_SIZE ||
        mmap_cur < PROC_MMAP_BASE || mmap_cur > PROC_MMAP_BASE + PROC_MMAP_SIZE ||
        sp < PROC_STACK_BASE + 0x4000 || sp > PROC_STACK_BASE + PROC_STACK_SIZE ||
        entry < PROC_IMG_BASE || entry >= cur->img_end)
        return -EINVAL;
    if (cur->uid != 0) {                                 /* RLIMIT_NPROC: its user's processes */
        uint64_t n = 0;
        for (int i = 0; i < NPROC; i++) n += procs[i].state == P_RUN && procs[i].uid == cur->uid;
        if (n >= cur->rlim[RLIMIT_NPROC_][0]) return -EAGAIN;
    }
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
    c->img_ino = cur->img_ino;
    c->nseg = cur->nseg;
    memcpy(c->seg, cur->seg, sizeof c->seg);
    c->sig_caught = cur->sig_caught;
    c->sig_ignored = cur->sig_ignored;
    c->async_ctl = cur->async_ctl;
    c->async_entry = cur->async_entry;
    memcpy(c->rlim, cur->rlim, sizeof c->rlim);
    c->nice = cur->nice;
    cred_copy(c, cur);
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
    proc_apply_nice(c);                 /* the slot may come from a process with another nice */

    /* The child starts at entry (which longjmps to the fork point) with a
     * temporary stack inside the copied memory */
    seL4_UserContext ctx;
    memset(&ctx, 0, sizeof ctx);
    arch_ctx_start(&ctx, entry, arch_call_sp(sp - 0x2000), fs_base);
    if (start_thread(cs, &ctx)) {
        proc_exit(c, SIGKILL);
        return -EAGAIN;
    }
    return c->pid;
#endif
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
        slot_halt(p->slot);
        procfs_account(p);
        mem_free_all(p->slot);
        slot_owner[p->slot] = NULL;
        p->slot = -1;
    }
    struct proc *saved = cur;
    cur = p;
    vfs_close_all(p);
    cur = saved;
    tty_session_leader_exit(p);            /* its terminal: SIGHUP to the foreground group */

    /* Children are handed to init; orphaned zombies are freed */
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state == P_FREE || q->ppid != p->pid || q == p) continue;
        if (q->state == P_ZOMBIE) free_proc(q);
        else { q->ppid = 1; q->orphan = true; }
    }

    p->state = P_ZOMBIE;
    p->status = status;
    if (status & 0x7f) slog("info", "killed", p->name, "\"pid\":%d,\"sig\":%d", p->pid, status & 0x7f);
    p->wait_child = false;

    if (p->pid == 1) {
        slog("warn", "init-restart", NULL, "\"status\":%d", status);
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
        if ((options & WUNTRACED) && q->stopped && !q->stop_reported) {
            q->stop_reported = true;
            microkit_mr_set(1, (q->stop_sig << 8) | 0x7f);     /* WIFSTOPPED */
            cur->wait_child = false;
            return q->pid;
        }
        if ((options & WCONTINUED) && !q->stopped && !q->cont_reported) {
            q->cont_reported = true;
            microkit_mr_set(1, 0xffff);                        /* WIFCONTINUED */
            cur->wait_child = false;
            return q->pid;
        }
    }
    if (!any) return -ECHILD;
    if (options & WNOHANG) return 0;
    cur->wait_child = true;
    return POSIX_RETRY;
}

/* ------------------------------------------------------------------ */
/* Resource limits                                                    */
/* ------------------------------------------------------------------ */

static void rlimit_defaults(struct proc *p)
{
    for (int r = 0; r < RLIM_NLIMITS_; r++) p->rlim[r][0] = p->rlim[r][1] = RLIM_INF;
    p->rlim[RLIMIT_NOFILE_][0] = p->rlim[RLIMIT_NOFILE_][1] = NFD;
    p->rlim[RLIMIT_STACK_][0] = p->rlim[RLIMIT_STACK_][1] = PROC_STACK_SIZE;
    p->rlim[RLIMIT_NPROC_][0] = p->rlim[RLIMIT_NPROC_][1] = DEFAULT_NPROC;
}

/* prlimit64: pid 0 = itself; another one only of its own (real) user, or by root. The old value in
 * MR1, MR2; set: soft and hard. Raising the hard limit is for root; NOFILE stops at NFD (the
 * table), STACK at the stack there is */
static long sys_prlimit(long pid, long res, long set, uint64_t soft, uint64_t hard)
{
    if (res < 0 || res >= RLIM_NLIMITS_) return -EINVAL;
    struct proc *p = pid ? proc_by_pid(pid) : cur;
    if (!p) return -ESRCH;
    if (p != cur && cur->euid != 0 && p->uid != cur->uid) return -EPERM;
    microkit_mr_set(1, p->rlim[res][0]);
    microkit_mr_set(2, p->rlim[res][1]);
    if (!set) return 0;
    if (soft > hard) return -EINVAL;
    if (hard > p->rlim[res][1] && cur->euid != 0) return -EPERM;
    if (res == RLIMIT_NOFILE_ && hard > NFD) return -EPERM;
    if (res == RLIMIT_STACK_ && hard > PROC_STACK_SIZE && hard != RLIM_INF) return -EPERM;
    p->rlim[res][0] = soft;
    p->rlim[res][1] = hard;
    return 0;
}

/* One more page for the process in slot s? Its own limit (RLIMIT_AS), and for users other than
 * root, all of their processes together at most 3/4 of the memory: root keeps the rest to log in
 * and stop them. Pages shared through memfd are the shared object's, counted once (mem.c) */
bool proc_page_allowed(int s)
{
    struct proc *p = proc_by_slot(s);
    if (!p) return true;
    uint64_t mine = mem_slot_pages(s);
    bool ok = p->rlim[RLIMIT_AS_][0] == RLIM_INF || (mine + 1) * 4096 <= p->rlim[RLIMIT_AS_][0];
    if (ok && p->uid != 0) {
        uint64_t theirs = 0;
        for (int i = 0; i < NPROC; i++)
            if (procs[i].state == P_RUN && procs[i].uid == p->uid && procs[i].slot >= 0)
                theirs += mem_slot_pages(procs[i].slot);
        ok = theirs + 1 <= mem_total() / 4096 * 3 / 4;
    }
    p->over_budget = !ok;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */
/* ------------------------------------------------------------------ */

static bool default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
}

/* A stopped or continued child: SIGCHLD to its parent, and its wait4 looks again */
static void tell_parent(struct proc *p)
{
    struct proc *parent = p->orphan ? NULL : proc_by_pid(p->ppid);
    if (!parent) return;
    proc_signal(parent, SIGCHLD);
    if (parent->wait_child) proc_wake(parent);
}

/* Job control. Another process's thread is suspended right away (seL4 restarts the call it
 * may have been queued for when it resumes). The caller itself finishes its call first: posix_server
 * keeps the result and parks it until SIGCONT (protected() in posix_server.c) */
static void proc_stop(struct proc *p, int sig)
{
    if (p->stopped || p->pid == 1) return;           /* init is never stopped */
    p->stopped = true;
    p->stop_sig = sig;
    p->stop_reported = false;
    p->cont_reported = true;
    p->sig_pending &= ~SIGBIT(SIGCONT);
    if (p == cur) p->stop_self = true;
    else if (p->slot >= 0) slot_suspend(p->slot);
    tell_parent(p);
}

static void proc_continue(struct proc *p)
{
    if (!p->stopped) return;
    p->stopped = false;
    p->cont_reported = false;
    p->sig_pending &= ~(SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU));
    if (p->slot >= 0) slot_resume(p->slot);   /* nothing if it was not suspended */
    proc_wake(p);                                    /* a parked call retries */
    tell_parent(p);
}

/* POSIX: a group none of whose members has a parent in the same session but outside the group.
 * Its processes are not stopped by the terminal's signals (nobody would continue them) */
bool proc_pgrp_orphaned(int pgid)
{
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_RUN || q->pgid != pgid) continue;
        struct proc *pp = q->orphan ? NULL : proc_by_pid(q->ppid);
        if (pp && pp->pgid != pgid && pp->sid == q->sid) return false;
    }
    return true;
}

/* A caught signal for a process that is running its own code, maybe in a loop with no calls:
 * if it is outside the runtime, its registers go onto its stack and it runs the runtime's entry
 * point, which fetches the signal and comes back (POSIX_SYS_ASYNC). Inside the runtime (or
 * parked in a call) the signal comes with the reply, as always */
static void async_signal(struct proc *p)
{
#ifdef __wasm__
    (void)p;                             /* the browser port: signals come with the next reply */
#else
    if (!p->async_ctl || p->slot < 0 || p == cur || p->blocked || p->stopped) return;
    seL4_CPtr tcb = slot_tcb(p->slot);
    seL4_TCB_Suspend(tcb);
    uint8_t *ctl = mem_has_page(p->slot, p->async_ctl) ? mem_page_ptr(p->slot, p->async_ctl) : NULL;
    seL4_UserContext r;
    if (!ctl || *(volatile long *)ctl != 0 ||
        seL4_TCB_ReadRegisters(tcb, false, 0, sizeof r / sizeof(seL4_Word), &r)) {
        seL4_TCB_Resume(tcb);
        return;
    }
#ifdef __aarch64__
    struct posix_async_frame f = { .pc = r.pc, .sp = r.sp, .spsr = r.spsr };
    f.x[0] = r.x0;
    f.x[1] = r.x1;
    f.x[2] = r.x2;
    f.x[3] = r.x3;
    f.x[4] = r.x4;
    f.x[5] = r.x5;
    f.x[6] = r.x6;
    f.x[7] = r.x7;
    f.x[8] = r.x8;
    f.x[9] = r.x9;
    f.x[10] = r.x10;
    f.x[11] = r.x11;
    f.x[12] = r.x12;
    f.x[13] = r.x13;
    f.x[14] = r.x14;
    f.x[15] = r.x15;
    f.x[16] = r.x16;
    f.x[17] = r.x17;
    f.x[18] = r.x18;
    f.x[19] = r.x19;
    f.x[20] = r.x20;
    f.x[21] = r.x21;
    f.x[22] = r.x22;
    f.x[23] = r.x23;
    f.x[24] = r.x24;
    f.x[25] = r.x25;
    f.x[26] = r.x26;
    f.x[27] = r.x27;
    f.x[28] = r.x28;
    f.x[29] = r.x29;
    f.x[30] = r.x30;
    uintptr_t frame = (r.sp - sizeof f) & ~15UL;           /* no red zone on aarch64 */
#else
    struct posix_async_frame f = {
        .rip = r.rip, .rsp = r.rsp, .rflags = r.rflags, .rax = r.rax, .rbx = r.rbx, .rcx = r.rcx,
        .rdx = r.rdx, .rsi = r.rsi, .rdi = r.rdi, .rbp = r.rbp, .r8 = r.r8, .r9 = r.r9, .r10 = r.r10,
        .r11 = r.r11, .r12 = r.r12, .r13 = r.r13, .r14 = r.r14, .r15 = r.r15,
    };
    uintptr_t frame = (r.rsp - 136 - sizeof f) & ~15UL;      /* below the red zone and rip's slot */
#endif
    if (frame < PROC_STACK_BASE + 0x1000 || !mem_copy_in(p->slot, frame, &f, sizeof f)) {
        slog("warn", "async-no-room", p->name, "\"pid\":%d", p->pid);
        seL4_TCB_Resume(tcb);
        return;
    }
#ifdef __aarch64__
    r.pc = p->async_entry;
    r.sp = frame;
#else
    r.rip = p->async_entry;
    r.rsp = frame;
#endif
    slog("debug", "async-signal", p->name, "\"pid\":%d", p->pid);
    seL4_TCB_WriteRegisters(tcb, true, 0, sizeof r / sizeof(seL4_Word), &r);   /* and resume */
#endif
}

#ifdef __aarch64__
/* POSIX_SYS_ASYNC_RETURN: the entry point has delivered the signals; every register back from
 * the frame (seL4 keeps the saved status register to what EL0 may set) and the thread runs on
 * where it was interrupted. The call itself gets no reply: the thread was restarted */
static long async_return(uintptr_t frame)
{
    struct posix_async_frame f;
    if (!mem_copy_from_user(cur->slot, frame, &f, sizeof f)) return -EFAULT;
    seL4_CPtr tcb = slot_tcb(cur->slot);
    seL4_TCB_Suspend(tcb);
    seL4_UserContext r;
    seL4_TCB_ReadRegisters(tcb, false, 0, sizeof r / sizeof(seL4_Word), &r);
    r.pc = f.pc;
    r.sp = f.sp;
    r.spsr = f.spsr;
    r.x0 = f.x[0];
    r.x1 = f.x[1];
    r.x2 = f.x[2];
    r.x3 = f.x[3];
    r.x4 = f.x[4];
    r.x5 = f.x[5];
    r.x6 = f.x[6];
    r.x7 = f.x[7];
    r.x8 = f.x[8];
    r.x9 = f.x[9];
    r.x10 = f.x[10];
    r.x11 = f.x[11];
    r.x12 = f.x[12];
    r.x13 = f.x[13];
    r.x14 = f.x[14];
    r.x15 = f.x[15];
    r.x16 = f.x[16];
    r.x17 = f.x[17];
    r.x18 = f.x[18];
    r.x19 = f.x[19];
    r.x20 = f.x[20];
    r.x21 = f.x[21];
    r.x22 = f.x[22];
    r.x23 = f.x[23];
    r.x24 = f.x[24];
    r.x25 = f.x[25];
    r.x26 = f.x[26];
    r.x27 = f.x[27];
    r.x28 = f.x[28];
    r.x29 = f.x[29];
    r.x30 = f.x[30];
    seL4_TCB_WriteRegisters(tcb, true, 0, sizeof r / sizeof(seL4_Word), &r);
    return 0;
}
#endif

void proc_signal(struct proc *p, int sig)
{
    if (!p || p->state != P_RUN || sig <= 0 || sig > 64) return;
    uint64_t bit = SIGBIT(sig);
    if (sig == SIGKILL) { proc_exit(p, sig); return; }
    if (sig == SIGCONT) proc_continue(p);            /* always, even when ignored or caught */
    if (sig == SIGSTOP) { proc_stop(p, sig); return; }
    if (p->sig_ignored & bit) return;
    if (p->sig_caught & bit) {
        p->sig_pending |= bit;
        proc_wake(p);                      /* if it was blocked, it retries and gets EINTR */
        async_signal(p);                   /* if it is running its own code */
        return;
    }
    if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {   /* default action: stop */
        if (!proc_pgrp_orphaned(p->pgid)) proc_stop(p, sig);
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
    int found = 0, allowed = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_RUN) continue;
        bool match = pid > 0 ? q->pid == pid : pid == 0 ? q->pgid == cur->pgid :
                     pid == -1 ? q->pid != 1 && q != cur : q->pgid == -pid;   /* -1: all but init and itself */
        if (!match) continue;
        found++;
        if (!cred_may_signal(cur, q)) continue;
        allowed++;
        proc_signal(q, sig);
    }
    return !found ? -ESRCH : !allowed ? -EPERM : 0;
}

/* getpriority/setpriority. which: 0 = process, 1 = group, 2 = user.
 * Like the Linux kernel, getpriority returns 20 - nice (musl converts it).
 * seL4 schedules by strict priority, not by shares as Linux does, so nice only moves a process
 * down: nice > 0 runs below the normal processes (a busy background job cannot slow down the
 * interactive ones); nice <= 0 is the normal priority (a busy process with a higher seL4
 * priority would stop every other one). The value itself is kept as given. */
static int prio_of_nice(int nice) { return nice > 0 ? PROC_PRIO_LOW : PROC_PRIO_NORMAL; }

void proc_apply_nice(struct proc *p)
{
    if (p->slot >= 0) mem_slot_set_prio(p->slot, prio_of_nice(p->nice));
}

static long sys_priority(bool set, long which, long who, long prio)
{
    if (which < 0 || which > 2) return -EINVAL;
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    int found = 0, best = 19;
    long err = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state != P_RUN) continue;
        bool match = which == 0 ? q->pid == (who ? who : cur->pid)
                   : which == 1 ? q->pgid == (who ? who : cur->pgid)
                   : q->uid == (uint32_t)(who ? who : (long)cur->euid);
        if (!match) continue;
        found++;
        if (set) {
            if (cur->euid != 0 && cur->euid != q->uid && cur->euid != q->euid) { err = -EPERM; continue; }
            if (cur->euid != 0 && prio < q->nice) { err = -EACCES; continue; }   /* only root lowers it */
            q->nice = prio;
            proc_apply_nice(q);
        } else if (q->nice < best) {
            best = q->nice;
        }
    }
    if (!found) return -ESRCH;
    if (set) return err;
    return 20 - best;
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
/* reboot(2): power off, restart or halt the machine                  */
/* ------------------------------------------------------------------ */

#define REBOOT_MAGIC1     0xfee1deadu
#define RB_AUTOBOOT       0x01234567u        /* restart */
#define RB_HALT_SYSTEM    0xcdef0123u
#define RB_POWER_OFF      0x4321fedcu
#define RB_ENABLE_CAD     0x89abcdefu        /* Ctrl-Alt-Del: accepted, nothing to do */
#define RB_DISABLE_CAD    0u
#define PM1A_IOPORT       1                  /* ioport ids in meta.py */
#define RESET_IOPORT      2

#ifdef __aarch64__
/* ARM: PSCI's SYSTEM_OFF and SYSTEM_RESET, an SMC call (meta.py gives this PD the right; QEMU's
 * virt with EL2 takes PSCI by SMC). It does not come back when it works */
#define PSCI_SYSTEM_OFF   0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u
static void psci(uint32_t fn)
{
    seL4_ARM_SMCContext args = { .x0 = fn }, resp = { 0 };
    microkit_arm_smc_call(&args, &resp);
}
#endif

/* The orderly part (services down, the VM, other processes) is done by /sbin/poweroff in user
 * space; here the file system is unmounted clean, every process is stopped and the machine goes
 * off. Power off writes SLP_EN with S5 into PM1a_CNT (QEMU q35: port 0x604, SLP_TYP 0; real
 * hardware will need \_S5 from the ACPI tables); restart uses the reset control register 0xCF9. */
static long sys_reboot(long magic, long cmd)
{
    if (cur->euid != 0) return -EPERM;
    if ((uint32_t)magic != REBOOT_MAGIC1) return -EINVAL;
    if ((uint32_t)cmd == RB_ENABLE_CAD || (uint32_t)cmd == RB_DISABLE_CAD) return 0;
    if ((uint32_t)cmd != RB_AUTOBOOT && (uint32_t)cmd != RB_HALT_SYSTEM && (uint32_t)cmd != RB_POWER_OFF)
        return -EINVAL;
    vfs_unmount();
    for (int i = 0; i < NPROC; i++)                    /* nobody runs (or writes) any more */
        if (procs[i].state == P_RUN && procs[i].slot >= 0) slot_halt(procs[i].slot);
    if ((uint32_t)cmd == RB_POWER_OFF) {
#if defined(__aarch64__)
        tty_puts("[posix] file system clean; powering off\n");
        psci(PSCI_SYSTEM_OFF);
#elif !defined(NO_ACPI_POWEROFF)
        tty_puts("[posix] file system clean; powering off\n");
        microkit_x86_ioport_write_16(PM1A_IOPORT, 0x604, 0x2000);       /* QEMU q35's PM1a control */
#else
        tty_puts("[posix] file system clean\n");      /* real hardware: no \_S5 from ACPI yet */
#endif
    } else if ((uint32_t)cmd == RB_AUTOBOOT) {
        tty_puts("[posix] file system clean; restarting\n");
#ifdef __x86_64__
        microkit_x86_ioport_write_8(RESET_IOPORT, 0xCF9, 0x02);
        microkit_x86_ioport_write_8(RESET_IOPORT, 0xCF9, 0x06);
#elif defined(__aarch64__)
        psci(PSCI_SYSTEM_RESET);
#endif
    }
    tty_puts("[posix] system halted: the machine can be switched off\n");
    return 0;                                          /* the caller stays stopped */
}

/* ------------------------------------------------------------------ */
/* Slot faults (they arrive through their fault endpoint)            */
/* ------------------------------------------------------------------ */

/* true: reply and the process carries on (the page now exists) */
/* ------------------------------------------------------------------ */
/* Program pages loaded when first touched                           */
/* ------------------------------------------------------------------ */

#define CLUSTER 0x8000UL                     /* read 32 KiB of the program at a time */
static uint8_t cluster_buf[CLUSTER];

/* Creates the missing pages of the 64 KiB cluster around addr that belong to p's segments, with
 * their contents from the program's file (zeros past the file data: .bss). Existing pages are
 * left alone (they may have been written). Returns false if out of memory or on a read error */
static bool image_cluster(struct proc *p, uintptr_t addr)
{
    uintptr_t c0 = addr & ~(CLUSTER - 1);
    uint32_t fresh = 0;                      /* bit i: page c0 + i * 4096 created here */
    for (int i = 0; i < (int)(CLUSTER / 4096); i++) {
        uintptr_t v = c0 + (uintptr_t)i * 4096;
        if (mem_has_page(p->slot, v)) continue;
        bool in = false;
        for (int k = 0; k < p->nseg && !in; k++)
            in = v + 4096 > p->seg[k].vaddr && v < p->seg[k].vaddr + p->seg[k].memsz;
        if (!in) continue;
        if (!mem_copy_in(p->slot, v, NULL, 4096)) return false;
        fresh |= 1u << i;
    }
    if (!fresh) return true;
    for (int k = 0; k < p->nseg; k++) {
        uintptr_t lo = p->seg[k].vaddr > c0 ? p->seg[k].vaddr : c0;
        uintptr_t hi = p->seg[k].vaddr + p->seg[k].filesz;
        if (hi > c0 + CLUSTER) hi = c0 + CLUSTER;
        if (lo >= hi) continue;
        long n = vfs_read_file(p->img_ino, p->seg[k].off + (lo - p->seg[k].vaddr), cluster_buf, hi - lo);
        if (n != (long)(hi - lo)) return false;
        for (uintptr_t v = lo; v < hi; ) {               /* only into the pages created above */
            uintptr_t end = (v | 0xfff) + 1 < hi ? (v | 0xfff) + 1 : hi;
            if (fresh & (1u << ((v - c0) >> 12)))
                memcpy(mem_page_ptr(p->slot, v), cluster_buf + (v - lo), end - v);
            v = end;
        }
    }
    for (int i = 0; i < (int)(CLUSTER / 4096); i++)   /* code written: the instruction cache (ARM) */
        if (fresh & (1u << i)) mem_sync_code(p->slot, c0 + (uintptr_t)i * 4096);
    return true;
}

/* A file is about to change (write, truncate, unlink, rename over it): every process running it
 * gets the rest of its pages now, so it never sees the new contents */
void proc_image_release(uint32_t ino)
{
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_RUN || p->slot < 0 || p->img_ino != ino) continue;
        bool ok = true;
        for (int k = 0; k < p->nseg && ok; k++)
            for (uintptr_t c = p->seg[k].vaddr & ~(CLUSTER - 1); ok && c < p->seg[k].vaddr + p->seg[k].memsz; c += CLUSTER)
                ok = image_cluster(p, c);
        p->img_ino = 0;
        if (!ok) {
            tty_puts("[posix] out of memory loading a program whose file changes: killed\n");
            proc_exit(p, SIGKILL);
        }
    }
}

#ifndef __wasm__                          /* the browser port: no page faults */
bool proc_fault(int slot, long label)
{
    struct proc *p = proc_by_slot(slot);
    if (!p) return false;
    /* The fault's registers first: loading a page calls fs_ext2, which overwrites them */
    uintptr_t a = label == seL4_Fault_VMFault ? microkit_mr_get(seL4_VMFault_Addr) : microkit_mr_get(0);
    uintptr_t pc = label == seL4_Fault_VMFault ? microkit_mr_get(seL4_VMFault_IP) : 0;
    long vec = label == seL4_Fault_UserException ? microkit_mr_get(seL4_UserException_Number) : 0;
    if (label == seL4_Fault_VMFault && p->img_ino &&
        a >= PROC_IMG_BASE && a < PROC_IMG_BASE + PROC_IMG_SIZE && !image_cluster(p, a)) {
        tty_puts("[posix] could not load a page of the program: killed\n");
        proc_exit(p, SIGBUS);
        return false;
    }
    if (label == seL4_Fault_VMFault && mem_fault(slot, a)) return true;
    if (label == seL4_Fault_VMFault && p->over_budget) {     /* a page it may not have */
        tty_puts("[posix] ");
        tty_puts(p->name);
        tty_puts(" (pid ");
        tty_putdec(p->pid);
        tty_puts("): out of memory (its limit, its user's, or the system's): killed\n");
        proc_exit(p, SIGKILL);
        return false;
    }
    /* Processor exception: as in Linux, general protection -> SIGSEGV, division -> SIGFPE */
    int sig = SIGILL;
    if (label == seL4_Fault_VMFault) sig = SIGSEGV;
    else if (label == seL4_Fault_UserException) {
        sig = vec == 0 ? SIGFPE : vec == 6 ? SIGILL : SIGSEGV;
    }
    slog("info", "fault", p->name, "\"pid\":%d,\"sig\":%d,\"addr\":%" PRIu64 ",\"pc\":%" PRIu64 ",\"label\":%ld",
         p->pid, sig, (uint64_t)a, (uint64_t)pc, label);
    tty_puts("[posix] ");
    tty_puts(p->name);
    tty_puts(" (pid ");
    tty_putdec(p->pid);
    tty_puts(sig == SIGSEGV ? "): segmentation fault at 0x" : sig == SIGFPE ? "): arithmetic error at 0x"
                                                             : "): illegal instruction at 0x");
    char hex[17];
    uint64_t v = a;
    for (int i = 15; i >= 0; i--, v >>= 4) hex[i] = "0123456789abcdef"[v & 15];
    hex[16] = 0;
    tty_puts(hex);
    if (pc) {                              /* where: the instruction (addr2line on the program) */
        v = pc;
        for (int i = 15; i >= 0; i--, v >>= 4) hex[i] = "0123456789abcdef"[v & 15];
        tty_puts(" (pc 0x");
        tty_puts(hex);
        tty_puts(")");
    }
#ifdef __aarch64__
    seL4_UserContext r;                    /* who jumped there: the link register */
    if (!seL4_TCB_ReadRegisters(slot_tcb(slot), false, 0, sizeof r / sizeof(seL4_Word), &r)) {
        v = r.x30;
        for (int i = 15; i >= 0; i--, v >>= 4) hex[i] = "0123456789abcdef"[v & 15];
        tty_puts(" (lr 0x");
        tty_puts(hex);
        tty_puts(")");
    }
#endif
    tty_puts("\n");
    proc_exit(p, sig);
    return false;                          /* no reply: the slot stays stopped */
}
#endif

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

bool proc_handles(long nr)
{
    switch (nr) {
    case __NR_fork: case __NR_vfork: case __NR_execve: case __NR_wait4: case __NR_exit: case __NR_exit_group:
    case __NR_getpid: case __NR_getppid: case __NR_kill:
    case __NR_setpgid: case __NR_getpgid: case __NR_getpgrp: case __NR_setsid: case __NR_getsid:
    case __NR_rt_sigaction: case __NR_rt_sigsuspend: case POSIX_SYS_ASYNC: case POSIX_SYS_ASYNC_RETURN: case __NR_prlimit64:
    case __NR_getpriority: case __NR_setpriority:
    case __NR_setitimer: case __NR_getitimer:
    case __NR_munmap: case __NR_sysinfo: case __NR_reboot:
        return true;
    }
    return false;
}

long proc_syscall(long nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5)
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
    case POSIX_SYS_ASYNC_RETURN:
#ifdef __aarch64__
        return async_return(a1);
#else
        return -ENOSYS;
#endif
    case POSIX_SYS_ASYNC:
        cur->async_ctl = a1;
        cur->async_entry = a2;
        return 0;
    case __NR_prlimit64:                   /* the runtime: pid, resource, set?, soft, hard */
        return sys_prlimit(a1, a2, a3, a4, a5);
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
    case __NR_reboot:     return sys_reboot(a1, a3);
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
    rlimit_defaults(p);
    cred_init_root(p);
    vfs_proc_tty(p);
    tty_console_session(0);                /* nobody's until a login (init: its own session) */

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
