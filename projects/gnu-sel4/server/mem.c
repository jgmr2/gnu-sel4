/*
 * posix_server's memory manager: process slots and their pages, on demand,
 * from the memory of its untypeds (<cap_untyped> in meta.py).
 *
 * Slot: TCB, address space (PML4 with an ASID), 128-entry CNode, notification
 * and a scheduling context from the pool Microkit provides. It is created the
 * first time it is used and then reused: the process's page tables stay for
 * the next one.
 *
 * Process page: two capabilities to the same frame, one mapped in the process
 * and the other in the slot's window in the server (PROC_WIN_BASE). A
 * directory per slot (vaddr < 4 GiB -> 1024 leaves of 1024 entries) records
 * which frame backs each page. Freed frames are kept on a stack and zeroed
 * when reused; new ones already come zeroed.
 *
 * CSpace: new capabilities go into 2^14-entry CNodes that the manager creates
 * and places in its root CNode (entries SRV_FREE_ROOT_SLOT..63). A CNode with
 * 2^r entries at root[n] has guard 58 - r, so the cptr (n << 58) | i names its
 * entry i, and (SRV_ROOT_CNODE_SLOT << 58) | n names the CNode itself.
 */
#include <microkit.h>
#include <string.h>
#include <sel4/sel4_arch/mapping.h>
#include "server.h"

#define ROOT_CNODE   (((seL4_CPtr)SRV_ROOT_CNODE_SLOT << 58) | SRV_ROOT_CNODE_SLOT)
#define ROOT_REF(n)  (((seL4_CPtr)SRV_ROOT_CNODE_SLOT << 58) | (n))   /* names root[n] */
#define ROOT_DEPTH   58                 /* guard + radix of any root entry */
#define SELF_VSPACE  3                  /* Microkit's PD_VSPACE_CAP_IDX */
#define SELF_TCB     ((seL4_CPtr)SRV_TCB_SLOT << 58)
#define ASID_POOL    ((seL4_CPtr)SRV_ASID_POOL_SLOT << 58)
#define SCHED_POOL(i) (((seL4_CPtr)SRV_SCHED_POOL_SLOT << 58) | (i))
#define PROC_PRIO    100                /* below posix_server (150) and above the VM (90) */

#define BADGE_PPC    (1ULL << 63)       /* libmicrokit's protected(): channel = badge & 0x3f */
#define BADGE_FAULT  (1ULL << 62)       /* libmicrokit's fault(): child = badge & 0xff */

/* ---------------- CSpace ---------------- */

#define CNODE_BITS   14
#define CAP_IDX(c)   ((uint32_t)(((c) >> 58) << CNODE_BITS | ((c) & ((1u << CNODE_BITS) - 1))))
#define CAP_PTR(x)   ((seL4_CPtr)((x) >> CNODE_BITS) << 58 | ((x) & ((1u << CNODE_BITS) - 1)))

static unsigned next_root = SRV_FREE_ROOT_SLOT;
static uint64_t ut_used;                /* bytes taken from the untypeds */
static unsigned cur_root;
static unsigned cur_used = 1u << CNODE_BITS;
static int cur_pool;

/* Creates an object at CSpace entry (n, i), from the first untyped that has room */
static seL4_Error ut_retype(seL4_Word type, seL4_Word bits, seL4_Word node, seL4_Word node_depth, seL4_Word i)
{
    for (; cur_pool < SRV_NPOOLS; cur_pool++) {
        seL4_CPtr pool = (seL4_CPtr)(SRV_POOL_SLOT + cur_pool) << 58;
        seL4_Error e = seL4_Untyped_Retype(pool, type, bits, ROOT_CNODE, node, node_depth, i, 1);
        if (e == seL4_NoError) {
            ut_used += type == seL4_CapTableObject ? 1UL << (bits + seL4_SlotBits)
                     : type == seL4_TCBObject ? 1UL << seL4_TCBBits
                     : type == seL4_NotificationObject ? 1UL << seL4_NotificationBits : 4096;
        }
        if (e != seL4_NotEnoughMemory) return e;
    }
    return seL4_NotEnoughMemory;
}

static bool new_cnode(void)
{
    if (next_root > 62) return false;              /* root[63]: temporary entry */
    unsigned n = next_root;
    if (ut_retype(seL4_CapTableObject, CNODE_BITS, 0, 0, 63)) return false;
    if (seL4_CNode_Mutate(ROOT_CNODE, n, ROOT_DEPTH, ROOT_CNODE, 63, ROOT_DEPTH,
                          seL4_CNode_CapData_new(0, ROOT_DEPTH - CNODE_BITS).words[0]))
        return false;
    next_root++;
    cur_root = n;
    cur_used = 0;
    return true;
}

/* n consecutive entries of the same CNode */
static seL4_CPtr cslots(unsigned n)
{
    if (cur_used + n > 1u << CNODE_BITS && !new_cnode()) return 0;
    seL4_CPtr c = (seL4_CPtr)cur_root << 58 | cur_used;
    cur_used += n;
    return c;
}

static seL4_CPtr make(seL4_Word type, seL4_Word bits)
{
    seL4_CPtr c = cslots(1);
    if (!c || ut_retype(type, bits, c >> 58, ROOT_DEPTH, c & ((1u << CNODE_BITS) - 1))) return 0;
    return c;
}

/* CNode operations on an entry of our CSpace: (root, index, depth) */
#define CN(c)  ROOT_REF((c) >> 58), ((c) & ((1u << CNODE_BITS) - 1)), ROOT_DEPTH

/* ---------------- mapping ---------------- */

/* Maps frame at vaddr in vspace, creating any missing page tables */
static bool map_frame(seL4_CPtr frame, seL4_CPtr vspace, uintptr_t vaddr, seL4_CapRights_t rights)
{
    for (;;) {
        seL4_Error e = seL4_X86_Page_Map(frame, vspace, vaddr, rights, seL4_X86_Default_VMAttributes);
        if (e == seL4_NoError) return true;
        if (e != seL4_FailedLookup) return false;
        seL4_Word type;
        switch (seL4_MappingFailedLookupLevel()) {
        case SEL4_MAPPING_LOOKUP_NO_PDPT: type = seL4_X86_PDPTObject; break;
        case SEL4_MAPPING_LOOKUP_NO_PD:   type = seL4_X86_PageDirectoryObject; break;
        case SEL4_MAPPING_LOOKUP_NO_PT:   type = seL4_X86_PageTableObject; break;
        default: return false;
        }
        seL4_CPtr pt = make(type, 0);
        if (!pt) return false;
        seL4_X86_VMAttributes a = seL4_X86_Default_VMAttributes;
        e = type == seL4_X86_PDPTObject ? seL4_X86_PDPT_Map(pt, vspace, vaddr, a)
          : type == seL4_X86_PageDirectoryObject ? seL4_X86_PageDirectory_Map(pt, vspace, vaddr, a)
          : seL4_X86_PageTable_Map(pt, vspace, vaddr, a);
        if (e) return false;
    }
}

/* The server's own memory, growing on demand (directory leaves) */
#define META_BASE 0x8000000000UL
static uintptr_t meta_top = META_BASE;

static void *meta_page(void)
{
    seL4_CPtr f = make(seL4_X86_4K, 0);
    if (!f || !map_frame(f, SELF_VSPACE, meta_top, seL4_ReadWrite)) return NULL;
    void *p = (void *)meta_top;
    meta_top += 4096;
    return p;
}

/* ---------------- frames ---------------- */

#define MAX_FRAMES (1u << 18)           /* 1 GiB of 4 KiB frames */
static uint32_t free_frames[MAX_FRAMES];
static uint32_t nfree;
unsigned long mem_pages_used;

/* A frame: entry x (the process's capability) and x + 1 (the window's).
 * *recycled says whether it has to be zeroed. */
static uint32_t frame_get(bool *recycled)
{
    if (nfree) {
        *recycled = true;
        return free_frames[--nfree];
    }
    *recycled = false;
    if (cur_used & 1) cur_used++;                  /* aligned pairs */
    seL4_CPtr c = cslots(2);
    if (!c) return 0;
    if (ut_retype(seL4_X86_4K, 0, c >> 58, ROOT_DEPTH, c & ((1u << CNODE_BITS) - 1))) {
        return 0;
    }
    if (seL4_CNode_Copy(CN(c + 1), CN(c), seL4_AllRights)) return 0;
    return CAP_IDX(c);
}

static void frame_put(uint32_t x)
{
    seL4_X86_Page_Unmap(CAP_PTR(x));
    seL4_X86_Page_Unmap(CAP_PTR(x) + 1);
    free_frames[nfree++] = x;
}

/* ---------------- slots ---------------- */

static struct slot {
    bool made;
    seL4_CPtr tcb, vspace, cnode, ntfn;
    uint32_t *dir[1024];                /* vaddr >> 22 -> leaf; leaf[(vaddr >> 12) & 1023] = frame */
    uint32_t npages;                    /* the process's pages (excluding the shared and IPC buffers) */
} slots[PROC_SLOTS];

static uint32_t *free_leaves;           /* list of free leaves (linked through their first word) */

uintptr_t proc_win(int s, uintptr_t vaddr)
{
    return PROC_WIN_BASE + ((uintptr_t)s << 32) + vaddr;
}

seL4_CPtr slot_tcb(int s) { return slots[s].tcb; }
void slot_signal(int s) { seL4_Signal(slots[s].ntfn); }

static uint32_t *leaf(struct slot *sl, uintptr_t v, bool create)
{
    uint32_t **d = &sl->dir[v >> 22];
    if (!*d && create) {
        if (free_leaves) {
            *d = free_leaves;
            free_leaves = *(uint32_t **)free_leaves;
            memset(*d, 0, 4096);
        } else {
            *d = meta_page();
        }
    }
    return *d;
}

static bool keep(uintptr_t v) { return v >= PROC_BUF_BASE && v < PROC_IPCBUF + 4096; }

/* ---------------- shared memory (memfd) ----------------
 *
 * A memfd is an object with its own pages (frame x of each one is not mapped
 * anywhere: it is the original). Each time a page is mapped into a process,
 * two copies of its capability are made (process and server window), in a
 * pair of CSpace entries; in the slot's directory the entry holds
 * SHARED | pair, and pair_meta[pair] says which memfd and page it is. */
#define SHARED       0x80000000u
#define SHARED_RO    0x40000000u
#define ENTRY_IDX    0x000fffffu
#define SHM_MAX      128
#define SHM_DIR      64                       /* x 1024 pages: up to 256 MiB per memfd */
#define SHM_SCRATCH  0x700000000000UL         /* server page used to read/write/clear */

static struct shm {
    bool used;
    int refs;                                 /* open file descriptions (vfs.c) */
    uint32_t maps;                            /* pages mapped into processes */
    uint32_t npages;
    uint32_t *dir[SHM_DIR];
} shms[SHM_MAX];

static uint32_t pair_meta[1u << 19];         /* (memfd << 20) | page, per CSpace pair */
static uint32_t free_pairs[4096], nfree_pairs;

static uint32_t *shm_slot(struct shm *m, uint32_t pg, bool create)
{
    uint32_t **d = &m->dir[pg >> 10];
    if (!*d && create) {
        if (free_leaves) {
            *d = free_leaves;
            free_leaves = *(uint32_t **)free_leaves;
            memset(*d, 0, 4096);
        } else {
            *d = meta_page();
        }
    }
    return *d ? &(*d)[pg & 1023] : NULL;
}

/* Copies or clears a memfd page through SHM_SCRATCH */
static bool shm_touch(uint32_t x, void *buf, size_t off, size_t len, int op)   /* 0 clear, 1 read, 2 write */
{
    seL4_CPtr w = CAP_PTR(x) + 1;
    if (!map_frame(w, SELF_VSPACE, SHM_SCRATCH, seL4_ReadWrite)) return false;
    char *pg = (char *)SHM_SCRATCH;
    if (op == 0) memset(pg, 0, 4096);
    else if (op == 1) memcpy(buf, pg + off, len);
    else memcpy(pg + off, buf, len);
    seL4_X86_Page_Unmap(w);
    return true;
}

static void shm_maybe_free(int id)
{
    struct shm *m = &shms[id];
    if (m->refs || m->maps) return;
    for (uint32_t pg = 0; pg < m->npages; pg++) {
        uint32_t *e = shm_slot(m, pg, false);
        if (e && *e) { frame_put(*e); mem_pages_used--; }
    }
    for (int d = 0; d < SHM_DIR; d++)
        if (m->dir[d]) { *(uint32_t **)m->dir[d] = free_leaves; free_leaves = m->dir[d]; m->dir[d] = NULL; }
    m->used = false;
}

long mem_shm_new(void)
{
    for (int i = 0; i < SHM_MAX; i++)
        if (!shms[i].used) {
            memset(&shms[i], 0, sizeof shms[i]);
            shms[i].used = true;
            shms[i].refs = 1;
            return i;
        }
    return -ENFILE;
}

void mem_shm_ref(int id) { shms[id].refs++; }
void mem_shm_unref(int id) { shms[id].refs--; shm_maybe_free(id); }
uint64_t mem_shm_size(int id) { return (uint64_t)shms[id].npages * 4096; }

/* ftruncate: growing creates zeroed pages; shrinking only if nobody has them mapped */
long mem_shm_truncate(int id, uint64_t size)
{
    struct shm *m = &shms[id];
    uint32_t want = (size + 4095) / 4096;
    if (want > SHM_DIR * 1024) return -EFBIG;
    while (m->npages < want) {
        uint32_t *e = shm_slot(m, m->npages, true);
        bool recycled;
        uint32_t x = e ? frame_get(&recycled) : 0;
        if (!x) return -ENOMEM;
        if (recycled && !shm_touch(x, NULL, 0, 0, 0)) { frame_put(x); return -ENOMEM; }
        *e = x;
        mem_pages_used++;
        m->npages++;
    }
    while (m->npages > want && !m->maps) {
        uint32_t *e = shm_slot(m, --m->npages, false);
        if (e && *e) { frame_put(*e); *e = 0; mem_pages_used--; }
    }
    return 0;
}

/* read/write/pread/pwrite of a memfd */
long mem_shm_rw(int id, uint64_t off, void *buf, size_t len, bool write)
{
    struct shm *m = &shms[id];
    uint64_t size = (uint64_t)m->npages * 4096;
    if (off >= size) return write ? -ENOSPC : 0;
    if (len > size - off) len = size - off;
    size_t done = 0;
    while (done < len) {
        uint32_t pg = (off + done) / 4096, po = (off + done) % 4096;
        size_t n = 4096 - po < len - done ? 4096 - po : len - done;
        uint32_t *e = shm_slot(m, pg, false);
        if (!e || !*e || !shm_touch(*e, (char *)buf + done, po, n, write ? 2 : 1)) break;
        done += n;
    }
    return done;
}

/* Maps memfd page pg at vaddr of slot s (two copies of its capability) */
static bool shared_map(int s, uintptr_t v, int id, uint32_t pg, bool ro)
{
    struct slot *sl = &slots[s];
    struct shm *m = &shms[id];
    uint32_t *src = shm_slot(m, pg, false);
    uint32_t *l = leaf(sl, v, true);
    if (!src || !*src || !l) return false;
    uint32_t *e = &l[(v >> 12) & 1023];
    if (*e) return false;
    seL4_CPtr c;
    if (nfree_pairs) {
        uint32_t x = free_pairs[--nfree_pairs];
        c = CAP_PTR(x);
    }
    else {
        if (cur_used & 1) cur_used++;
        c = cslots(2);
        if (!c) return false;
    }
    seL4_CPtr master = CAP_PTR(*src);
    if (seL4_CNode_Copy(CN(c), CN(master), seL4_AllRights) ||
        seL4_CNode_Copy(CN(c + 1), CN(master), seL4_AllRights) ||
        !map_frame(c + 1, SELF_VSPACE, proc_win(s, v), seL4_ReadWrite) ||
        !map_frame(c, sl->vspace, v, ro ? seL4_CanRead : seL4_ReadWrite)) {
        seL4_CNode_Delete(CN(c));
        seL4_CNode_Delete(CN(c + 1));
        free_pairs[nfree_pairs++] = CAP_IDX(c);
        return false;
    }
    pair_meta[CAP_IDX(c) >> 1] = (uint32_t)id << 20 | pg;
    *e = SHARED | (ro ? SHARED_RO : 0) | CAP_IDX(c);
    m->maps++;
    return true;
}

static void shared_unmap(uint32_t entry)
{
    uint32_t pair = entry & ENTRY_IDX;
    seL4_CPtr c = CAP_PTR(pair);
    seL4_X86_Page_Unmap(c);
    seL4_X86_Page_Unmap(c + 1);
    seL4_CNode_Delete(CN(c));
    seL4_CNode_Delete(CN(c + 1));
    if (nfree_pairs < sizeof free_pairs / sizeof *free_pairs) free_pairs[nfree_pairs++] = pair;
    int id = pair_meta[pair >> 1] >> 20;
    shms[id].maps--;
    shm_maybe_free(id);
}

/* mmap(MAP_SHARED) of a memfd: [vaddr, vaddr + len) <- pages from off */
long mem_shm_map(int s, int id, uintptr_t vaddr, size_t len, uint64_t off, bool write)
{
    struct shm *m = &shms[id];
    if ((vaddr | off) & 0xfff) return -EINVAL;
    if (vaddr < PROC_MMAP_BASE || vaddr + len > PROC_MMAP_BASE + PROC_MMAP_SIZE) return -ENOMEM;
    uint32_t first = off / 4096, n = (len + 4095) / 4096;
    if (first + n > m->npages) return -ENXIO;           /* beyond the memfd's size */
    mem_free_range(s, vaddr, (size_t)n * 4096);          /* replaces whatever was there */
    for (uint32_t i = 0; i < n; i++) {
        if (!shared_map(s, vaddr + (uintptr_t)i * 4096, id, first + i, !write)) {
            mem_free_range(s, vaddr, (size_t)i * 4096);
            return -ENOMEM;
        }
    }
    return 0;
}

uint32_t mem_slot_pages(int s) { return slots[s].npages; }

uint64_t mem_slot_consumed_us(int s)
{
    seL4_SchedContext_Consumed_t c = seL4_SchedContext_Consumed(SCHED_POOL(s));
    return c.error ? 0 : c.consumed;
}

/* Process page at vaddr (created if it does not exist). zero = clear it if it is recycled. */
static bool page(int s, uintptr_t v, bool zero)
{
    struct slot *sl = &slots[s];
    v &= ~0xfffUL;
    uint32_t *l = leaf(sl, v, true);
    if (!l) return false;
    uint32_t *e = &l[(v >> 12) & 1023];
    if (*e) return true;
    bool recycled;
    uint32_t x = frame_get(&recycled);
    if (!x) return false;
    seL4_CPtr c = CAP_PTR(x);
    if (!map_frame(c + 1, SELF_VSPACE, proc_win(s, v), seL4_ReadWrite) ||
        !map_frame(c, sl->vspace, v, seL4_ReadWrite)) {
        frame_put(x);
        return false;
    }
    if (recycled && zero) memset((void *)proc_win(s, v), 0, 4096);
    *e = x;
    mem_pages_used++;
    if (!keep(v)) sl->npages++;
    return true;
}

bool mem_ensure(int s, uintptr_t vaddr, size_t len)
{
    for (uintptr_t v = vaddr & ~0xfffUL; v < vaddr + len; v += 4096)
        if (!page(s, v, true)) return false;
    return true;
}


void mem_free_range(int s, uintptr_t vaddr, size_t len)
{
    struct slot *sl = &slots[s];
    uintptr_t end = vaddr + len;
    if (end > 1UL << 32 || end < vaddr) end = 1UL << 32;
    for (uintptr_t v = vaddr & ~0xfffUL; v < end; ) {
        uint32_t *l = sl->dir[v >> 22];
        if (!l) { v = (v | 0x3fffff) + 1; continue; }
        uint32_t *e = &l[(v >> 12) & 1023];
        if (*e && (*e & SHARED)) {
            shared_unmap(*e);
            *e = 0;
        } else if (*e && !keep(v)) {
            frame_put(*e);
            *e = 0;
            mem_pages_used--;
            sl->npages--;
        }
        v += 4096;
    }
}

/* Everything but the shared buffer and the IPC buffer (exit and exec) */
void mem_free_all(int s)
{
    struct slot *sl = &slots[s];
    mem_free_range(s, 0, 1UL << 32);
    for (int d = 0; d < 1024; d++) {
        if (!sl->dir[d] || d == (PROC_BUF_BASE >> 22)) continue;
        *(uint32_t **)sl->dir[d] = free_leaves;
        free_leaves = sl->dir[d];
        sl->dir[d] = NULL;
    }
}

/* fork: copies every page of slot ps into slot cs */
bool mem_fork(int cs, int ps)
{
    struct slot *p = &slots[ps];
    for (int d = 0; d < 1024; d++) {
        if (!p->dir[d]) continue;
        for (int i = 0; i < 1024; i++) {
            if (!p->dir[d][i]) continue;
            uintptr_t v = (uintptr_t)d << 22 | (uintptr_t)i << 12;
            if (keep(v)) continue;
            uint32_t e = p->dir[d][i];
            if (e & SHARED) {                            /* MAP_SHARED: the child sees the same pages */
                uint32_t meta = pair_meta[(e & ENTRY_IDX) >> 1];
                if (!shared_map(cs, v, meta >> 20, meta & 0xfffff, e & SHARED_RO)) return false;
                continue;
            }
            if (!page(cs, v, false)) return false;
            memcpy((void *)proc_win(cs, v), (void *)proc_win(ps, v), 4096);
        }
    }
    return true;
}

/* Page fault of the process in slot s: if it falls in a region, the page is created */
bool mem_fault(int s, uintptr_t addr)
{
    bool ok = (addr >= PROC_IMG_BASE && addr < PROC_IMG_BASE + PROC_IMG_SIZE) ||
              (addr >= PROC_HEAP_BASE && addr < PROC_HEAP_BASE + PROC_HEAP_SIZE) ||
              (addr >= PROC_STACK_BASE && addr < PROC_STACK_BASE + PROC_STACK_SIZE) ||
              (addr >= PROC_MMAP_BASE && addr < PROC_MMAP_BASE + PROC_MMAP_SIZE);
    return ok && page(s, addr, true);
}

/* Creates the slot's objects the first time it is used */
bool slot_make(int s)
{
    struct slot *sl = &slots[s];
    if (sl->made) return true;
    sl->tcb = make(seL4_TCBObject, 0);
    sl->vspace = make(seL4_X64_PML4Object, 0);
    sl->cnode = make(seL4_CapTableObject, PROC_CNODE_BITS);
    sl->ntfn = make(seL4_NotificationObject, 0);
    seL4_CPtr fault_ep = cslots(1);
    if (!sl->tcb || !sl->vspace || !sl->cnode || !sl->ntfn || !fault_ep) return false;
    seL4_CapRights_t call = seL4_CapRights_new(1, 0, 0, 1);       /* grant_reply + write */
    if (seL4_X86_ASIDPool_Assign(ASID_POOL, sl->vspace) ||
        seL4_CNode_Mint(CN(fault_ep), ROOT_CNODE, SRV_EP_SLOT, ROOT_DEPTH, call, BADGE_FAULT | s) ||
        seL4_CNode_Mint(sl->cnode, PROC_EP_CAP, PROC_CNODE_BITS, ROOT_CNODE, SRV_EP_SLOT, ROOT_DEPTH,
                        call, BADGE_PPC | s) ||
        seL4_CNode_Copy(sl->cnode, PROC_NTFN_CAP, PROC_CNODE_BITS, CN(sl->ntfn), seL4_AllRights))
        return false;
    /* Shared and IPC buffers: fixed for the slot's whole lifetime */
    if (!mem_ensure(s, PROC_BUF_BASE, POSIX_BUF_SIZE + 4096)) return false;
    uint32_t ipcbuf = sl->dir[PROC_IPCBUF >> 22][(PROC_IPCBUF >> 12) & 1023];
    if (seL4_TCB_SetSpace(sl->tcb, fault_ep, sl->cnode,
                          seL4_CNode_CapData_new(0, 64 - PROC_CNODE_BITS).words[0], sl->vspace, 0) ||
        seL4_TCB_SetIPCBuffer(sl->tcb, PROC_IPCBUF, CAP_PTR(ipcbuf)) ||
        seL4_TCB_SetSchedParams(sl->tcb, SELF_TCB, 0, PROC_PRIO, SCHED_POOL(s), fault_ep))
        return false;
    sl->made = true;
    return true;
}

/* Process memory: total and free (unused or returned), in bytes */
uint64_t mem_total(void) { return SRV_POOL_BYTES; }
uint64_t mem_free(void) { return SRV_POOL_BYTES - ut_used + (uint64_t)nfree * 4096; }

void mem_init(void)
{
    if (!new_cnode()) tty_puts("[mem] ERROR: could not create the first CNode\n");
}
