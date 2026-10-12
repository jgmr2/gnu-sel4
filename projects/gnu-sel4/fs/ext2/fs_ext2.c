/*
 * fs_ext2: the ext2 file system server of GNU/seL4.
 *
 * It is an isolated component: it receives operations from the VFS
 * (posix_server) through protected calls (include/sel4posix/fs_protocol.h) and
 * reads/writes disk blocks as a client of the sDDF block subsystem (blk_virt).
 *
 * Current scope:
 *   - ext2 revision 1 with 4 KiB blocks (1 ext2 block = 1 sDDF block)
 *   - features: filetype, sparse_super, large_file
 *     (dir_index and resize_inode are tolerated; modifying an indexed
 *     directory drops its index and leaves it as a linear directory)
 *   - read and write: files, directories, symbolic links, hard links,
 *     create, delete, rename, truncate, chmod, chown, statfs
 *   - block cache, written at the end of every operation (all of its blocks at once), no journal
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <os/sddf.h>
#include <sddf/blk/queue.h>
#include <sddf/blk/config.h>
#include <sddf/blk/storage_info.h>
#include <sel4posix/fs_protocol.h>

/* Linux errno values */
#define ENOENT        2
#define EIO           5
#define EPERM         1
#define EBUSY        16
#define EUCLEAN     117
#define EACCES       13
#define EEXIST        17
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define EFBIG         27
#define ENOSPC        28
#define EROFS         30
#define EMLINK        31
#define ENAMETOOLONG  36
#define ENOTEMPTY     39
#define ELOOP         40

/* st_mode */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

/* Time for timestamps: posix_server provides it (FS_OP_SETCLOCK);
 * until then, the build time */
#ifndef BUILD_TIME
#define BUILD_TIME 0
#endif
static uint64_t clk_epoch = BUILD_TIME, clk_tsc0, clk_hz;

static inline uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static uint32_t now(void)
{
    if (!clk_hz) return clk_epoch;
    return clk_epoch + (tsc() - clk_tsc0) / clk_hz;
}

/* Counters for /proc/perf (FS_OP_STATS): count and TSC cycles */
struct ctr { uint64_t n, cyc; };
static struct ctr st_op[32], st_disk_read, st_disk_write, st_cache_lookup, st_flush;
static uint64_t st_hits, st_misses;

/* ------------------------------------------------------------------ */
/* On-disk ext2 structures                                            */
/* ------------------------------------------------------------------ */

#define EXT2_MAGIC        0xEF53
#define EXT2_ROOT_INO     2
#define BS                4096           /* supported block size */
#define APB               (BS / 4)       /* pointers per indirect block */
#define EXT2_NDIR_BLOCKS  12
#define EXT2_INDEX_FL     0x1000

#define INCOMPAT_FILETYPE     0x0002
#define RO_COMPAT_SPARSE      0x0001
#define RO_COMPAT_LARGE_FILE  0x0002
#define RO_COMPAT_BTREE_DIR   0x0004

struct ext2_super {
    uint32_t s_inodes_count, s_blocks_count, s_r_blocks_count;
    uint32_t s_free_blocks_count, s_free_inodes_count;
    uint32_t s_first_data_block, s_log_block_size, s_log_frag_size;
    uint32_t s_blocks_per_group, s_frags_per_group, s_inodes_per_group;
    uint32_t s_mtime, s_wtime;
    uint16_t s_mnt_count;
    int16_t  s_max_mnt_count;
    uint16_t s_magic, s_state, s_errors, s_minor_rev_level;
    uint32_t s_lastcheck, s_checkinterval, s_creator_os, s_rev_level;
    uint16_t s_def_resuid, s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size, s_block_group_nr;
    uint32_t s_feature_compat, s_feature_incompat, s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
};

struct ext2_gd {
    uint32_t bg_block_bitmap, bg_inode_bitmap, bg_inode_table;
    uint16_t bg_free_blocks_count, bg_free_inodes_count, bg_used_dirs_count, bg_pad;
    uint32_t bg_reserved[3];
};

struct ext2_inode {
    uint16_t i_mode, i_uid;
    uint32_t i_size, i_atime, i_ctime, i_mtime, i_dtime;
    uint16_t i_gid, i_links_count;
    uint32_t i_blocks, i_flags, i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation, i_file_acl, i_size_high, i_faddr;
    uint8_t  i_frag, i_fsize;
    uint16_t i_pad1, i_uid_high, i_gid_high;
    uint32_t i_reserved2;
};

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len, file_type;
    char     name[];
};

#define FT_UNKNOWN 0
#define FT_REG     1
#define FT_DIR     2
#define FT_FIFO    5
#define FT_SYMLINK 7

_Static_assert(sizeof(struct ext2_gd) == 32, "ext2_gd");
_Static_assert(sizeof(struct ext2_inode) == 128, "ext2_inode");

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Block client configuration (written by the build with objcopy) */
__attribute__((__section__(".blk_client_config"))) blk_client_config_t blk_config;
static blk_queue_handle_t blkq;
static uint64_t disk_blocks;           /* partition capacity in 4 KiB blocks */

/* Buffer shared with the VFS (set by Microkit, see meta.py) */
uintptr_t fs_buf;

static bool mounted, read_only;
static bool ro_features;               /* unknown read-only-compatible features: never read-write */
static bool fs_dirty;                  /* not cleanly unmounted: read-only until e2fsck checks it */
static uint8_t sb_raw[1024];           /* copy of the superblock (bytes 1024..2047 of the disk) */
static struct ext2_super *sb = (struct ext2_super *)sb_raw;
static uint32_t ngroups, inode_size;
#define MAX_GROUPS 1024
static struct ext2_gd gd[MAX_GROUPS];
static bool sb_dirty, gd_dirty;
/* Only the free block/inode counts changed: written at sync and unmount, not after every
 * operation. After a power cut the disk is marked in use and mount recounts them from the
 * bitmaps (recount_free), as Linux's ext2 does with its lazy counters */
static bool counts_dirty;

/* ------------------------------------------------------------------ */
/* Block cache over the sDDF data region                               */
/* ------------------------------------------------------------------ */

/* The cache: a hash table to find a block, an LRU list to pick the one to reuse. Writes are
 * write-back within one operation: bwrite() only marks the block dirty, and flush_dirty() (at the
 * end of every operation, before the reply) writes all of them at once, sorted, with runs of
 * consecutive blocks in one request and every request in flight together. So an operation is
 * still on the disk when it returns, but a block written several times in it (a bitmap, an
 * indirect block, a new block zeroed then filled) goes out once. The last STAGE_BLOCKS of the
 * data region are not cache: they stage multi-block transfers (contiguous in memory).
 * Order: the blocks allocated in the operation ("fresh": data, new indirect and directory
 * blocks) are written and completed first, everything else (inodes, bitmaps, blocks that
 * already existed, superblock, descriptors) after. Cut in between, the new blocks are only
 * unreferenced free space; nothing on the disk points to contents that were never written
 * (another file's old data). What a cut in the second half leaves, e2fsck repairs at boot. */
#define MAX_SLOTS    8192               /* 32 MiB of cache (meta.py's FS_CACHE_SIZE) */
#define STAGE_BLOCKS 256                   /* 1 MiB: the largest single disk request */
#define NO_SLOT      0xffffffffu
static struct cslot {
    uint64_t blk;
    uint32_t prev, next;               /* LRU list (head = most recently used) */
    uint32_t hnext;                    /* hash chain */
    bool valid, dirty, hashed;
    bool fresh;                        /* allocated in this operation: written first */
} slots[MAX_SLOTS];
#define HASH_SIZE (2 * MAX_SLOTS)
static uint32_t hash_head[HASH_SIZE];
static uint32_t nslots, lru_head = NO_SLOT, lru_tail = NO_SLOT;
static uint32_t dirty_list[MAX_SLOTS], ndirty;
static uint32_t stage_first;           /* first block of the staging area in the data region */

#ifdef FS_RAMDISK
/* Live systems on real hardware (make PLATFORM=...): no disk driver at all. The file system is
 * an ext2 image linked into this component (fs/ext2/ramdisk.S) and lives in RAM; nothing is ever
 * written to a real disk. The block "requests" below are memcpys between it and the cache */
extern uint8_t ramdisk_start[], ramdisk_end[];
#define RAM_CACHE_SIZE 0x400000             /* the disk is RAM too: a small cache is enough */
static uint8_t ram_cache[RAM_CACHE_SIZE] __attribute__((aligned(4096)));
static uint8_t *slot_data(int s) { return ram_cache + (uintptr_t)s * BS; }
static void ram_copy(blk_req_code_t code, uintptr_t offset, uint64_t blk, uint32_t count)
{
    uint8_t *disk = ramdisk_start + blk * BS, *mem = ram_cache + offset;
    if (code == BLK_REQ_READ) memcpy(mem, disk, (size_t)count * BS);
    else memcpy(disk, mem, (size_t)count * BS);
}
#else
static uint8_t *slot_data(int s) { return (uint8_t *)blk_config.data.vaddr + (uintptr_t)s * BS; }
#endif
static uint8_t *stage_data(uint32_t i) { return slot_data(stage_first + i); }
static uint32_t hash_of(uint64_t blk) { return (uint32_t)(blk * 0x9E3779B1u) % HASH_SIZE; }

static void lru_unlink(uint32_t i)
{
    struct cslot *c = &slots[i];
    if (c->prev != NO_SLOT) slots[c->prev].next = c->next; else lru_head = c->next;
    if (c->next != NO_SLOT) slots[c->next].prev = c->prev; else lru_tail = c->prev;
}

static void lru_push_front(uint32_t i)
{
    slots[i].prev = NO_SLOT;
    slots[i].next = lru_head;
    if (lru_head != NO_SLOT) slots[lru_head].prev = i;
    lru_head = i;
    if (lru_tail == NO_SLOT) lru_tail = i;
}

static void hash_remove(uint32_t i)
{
    if (!slots[i].hashed) return;
    uint32_t *pp = &hash_head[hash_of(slots[i].blk)];
    while (*pp != NO_SLOT && *pp != i) pp = &slots[*pp].hnext;
    if (*pp == i) *pp = slots[i].hnext;
    slots[i].hashed = false;
}

static void cache_init(void)
{
    for (uint32_t h = 0; h < HASH_SIZE; h++) hash_head[h] = NO_SLOT;
    for (uint32_t i = 0; i < nslots; i++) {
        slots[i] = (struct cslot){ .hnext = NO_SLOT };
        lru_push_front(i);
    }
}

/* One request at a time: queue it, notify blk_virt and wait for the reply.
 * blk_virt and the driver have a higher priority, so they run while this
 * component waits. Used for single reads; writes go through flush_dirty(). */
static int blk_io(blk_req_code_t code, int s, uint64_t blk)
{
    if (blk >= disk_blocks) return -EIO;
    uint64_t t0 = tsc();
#ifdef FS_RAMDISK
    ram_copy(code, (uintptr_t)s * BS, blk, 1);
    struct ctr *rc = code == BLK_REQ_READ ? &st_disk_read : &st_disk_write;
    rc->n++;
    rc->cyc += tsc() - t0;
    return 0;
#endif
    if (blk_enqueue_req(&blkq, code, (uintptr_t)s * BS, blk, 1, 0)) return -EIO;
    sddf_notify(blk_config.virt.id);
    while (blk_queue_empty_resp(&blkq)) __asm__ volatile("pause" ::: "memory");
    struct ctr *c = code == BLK_REQ_READ ? &st_disk_read : &st_disk_write;
    c->n++;
    c->cyc += tsc() - t0;
    blk_resp_status_t status;
    uint16_t count;
    uint32_t id;
    blk_dequeue_resp(&blkq, &status, &count, &id);
    return status == BLK_RESP_OK ? 0 : -EIO;
}

/* A batch of requests in flight together: queued until the queue is full, then all waited for */
#define MAX_INFLIGHT 32
static uint32_t batch_inflight;
static int batch_err;
static struct ctr *batch_ctr;
static uint64_t batch_t0;

static void batch_wait_all(void)
{
    if (!batch_inflight) return;
    sddf_notify(blk_config.virt.id);
    while (batch_inflight) {
        while (blk_queue_empty_resp(&blkq)) __asm__ volatile("pause" ::: "memory");
        blk_resp_status_t status;
        uint16_t count;
        uint32_t id;
        blk_dequeue_resp(&blkq, &status, &count, &id);
        if (status != BLK_RESP_OK) batch_err = -EIO;
        batch_inflight--;
    }
}

static void batch_begin(struct ctr *c) { batch_err = 0; batch_ctr = c; batch_t0 = tsc(); }

static void batch_add(blk_req_code_t code, uintptr_t offset, uint64_t blk, uint16_t count)
{
    if (blk + count > disk_blocks) { batch_err = -EIO; return; }
#ifdef FS_RAMDISK
    ram_copy(code, offset, blk, count);
    batch_ctr->n++;
    return;
#endif
    /* At most MAX_INFLIGHT requests: the driver's queue is shared with the Linux VM's disk */
    if (blk_queue_full_req(&blkq) || batch_inflight >= MAX_INFLIGHT) batch_wait_all();
    if (blk_enqueue_req(&blkq, code, offset, blk, count, 0)) { batch_err = -EIO; return; }
    batch_inflight++;
    batch_ctr->n++;
}

static int batch_end(void)
{
    batch_wait_all();
    batch_ctr->cyc += tsc() - batch_t0;
    return batch_err;
}

static void sort_by_block(uint32_t *v, uint32_t n)
{
    for (uint32_t gap = n / 2; gap; gap /= 2)                  /* shell sort: n is small */
        for (uint32_t i = gap; i < n; i++)
            for (uint32_t j = i; j >= gap && slots[v[j - gap]].blk > slots[v[j]].blk; j -= gap) {
                uint32_t t = v[j]; v[j] = v[j - gap]; v[j - gap] = t;
            }
}

/* Writes every dirty block: sorted, consecutive blocks in one request through the staging area */
/* Writes the slots of v[] (sorted by block) and waits for all of them */
static int write_slots(const uint32_t *v, uint32_t n)
{
    batch_begin(&st_disk_write);
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && j - i < STAGE_BLOCKS && slots[v[j]].blk == slots[v[j - 1]].blk + 1) j++;
        if (j - i == 1) {
            batch_add(BLK_REQ_WRITE, (uintptr_t)v[i] * BS, slots[v[i]].blk, 1);
        } else {
            batch_wait_all();                                  /* the staging area is free */
            for (uint32_t k = i; k < j; k++) memcpy(stage_data(k - i), slot_data(v[k]), BS);
            batch_add(BLK_REQ_WRITE, (uintptr_t)stage_first * BS, slots[v[i]].blk, j - i);
            batch_wait_all();                                  /* before the staging is reused */
        }
        i = j;
    }
    return batch_end();
}

static int flush_dirty(void)
{
    if (!ndirty) return 0;
    /* fresh blocks to the front (stable: each half stays sorted), written before the rest */
    static uint32_t ordered[MAX_SLOTS];
    uint32_t nf = 0, n = 0;
    sort_by_block(dirty_list, ndirty);
    for (uint32_t k = 0; k < ndirty; k++) if (slots[dirty_list[k]].fresh) ordered[n++] = dirty_list[k];
    nf = n;
    for (uint32_t k = 0; k < ndirty; k++) if (!slots[dirty_list[k]].fresh) ordered[n++] = dirty_list[k];
    int r = nf ? write_slots(ordered, nf) : 0;
    if (!r && n > nf) r = write_slots(ordered + nf, n - nf);
    for (uint32_t k = 0; k < ndirty; k++) slots[dirty_list[k]].dirty = slots[dirty_list[k]].fresh = false;
    ndirty = 0;
    return r;
}

/* The slot for blk: found (hit) or the least recently used one, emptied for it */
static int cache_slot_find(uint64_t blk, bool *hit)
{
    for (uint32_t i = hash_head[hash_of(blk)]; i != NO_SLOT; i = slots[i].hnext) {
        if (slots[i].blk == blk && slots[i].hashed) {
            lru_unlink(i);
            lru_push_front(i);
            *hit = slots[i].valid;
            return i;
        }
    }
    *hit = false;
    uint32_t v = lru_tail;
    if (slots[v].dirty) flush_dirty();          /* only with more dirty blocks than the cache */
    hash_remove(v);
    lru_unlink(v);
    lru_push_front(v);
    slots[v].valid = false;
    slots[v].fresh = false;
    slots[v].blk = blk;
    slots[v].hnext = hash_head[hash_of(blk)];
    hash_head[hash_of(blk)] = v;
    slots[v].hashed = true;
    return v;
}

static int cache_slot(uint64_t blk, bool *hit)
{
    uint64_t t0 = tsc();
    int s = cache_slot_find(blk, hit);
    st_cache_lookup.n++;
    st_cache_lookup.cyc += tsc() - t0;
    if (*hit) st_hits++; else st_misses++;
    return s;
}

/* Block in the cache (read from disk if needed). Returns the slot or -EIO. */
static int bread(uint64_t blk)
{
    bool hit;
    int s = cache_slot(blk, &hit);
    if (!hit) {
        if (blk_io(BLK_REQ_READ, s, blk)) { hash_remove(s); return -EIO; }
        slots[s].valid = true;
    }
    return s;
}

/* Reads the blocks of v[] that are not cached yet, together: runs of consecutive blocks in one
 * request through the staging area. Zero entries (holes) are skipped. */
static int bread_many(const uint32_t *v, uint32_t n)
{
    static uint32_t miss[STAGE_BLOCKS * 4];
    uint32_t nm = 0;
    for (uint32_t i = 0; i < n && nm < sizeof miss / sizeof *miss; i++) {
        if (!v[i]) continue;
        bool dup = false;
        for (uint32_t k = 0; k < nm && !dup; k++) dup = slots[miss[k]].blk == v[i];
        if (dup) continue;
        bool hit;
        int s = cache_slot(v[i], &hit);
        if (!hit) miss[nm++] = s;
    }
    if (!nm) return 0;
    sort_by_block(miss, nm);
    batch_begin(&st_disk_read);
    uint32_t i = 0;
    while (i < nm) {
        uint32_t j = i + 1;
        while (j < nm && j - i < STAGE_BLOCKS && slots[miss[j]].blk == slots[miss[j - 1]].blk + 1) j++;
        if (j - i == 1) {
            batch_add(BLK_REQ_READ, (uintptr_t)miss[i] * BS, slots[miss[i]].blk, 1);
        } else {
            batch_wait_all();
            batch_add(BLK_REQ_READ, (uintptr_t)stage_first * BS, slots[miss[i]].blk, j - i);
            batch_wait_all();
            for (uint32_t k = i; k < j; k++) memcpy(slot_data(miss[k]), stage_data(k - i), BS);
        }
        i = j;
    }
    int r = batch_end();
    for (uint32_t k = 0; k < nm; k++) {
        if (r) hash_remove(miss[k]);
        else slots[miss[k]].valid = true;
    }
    return r;
}

/* Block about to be overwritten completely: in the cache without reading it */
static int bget(uint64_t blk)
{
    bool hit;
    int s = cache_slot(blk, &hit);
    slots[s].valid = true;
    return s;
}

/* New block: not read, filled with zeros */
static int bzero_get(uint64_t blk)
{
    bool hit;
    int s = cache_slot(blk, &hit);
    memset(slot_data(s), 0, BS);
    slots[s].valid = true;
    return s;
}

/* Marks the block dirty: flush_dirty() writes it at the end of the operation */
static int bwrite(int s)
{
    if (!slots[s].dirty) {
        slots[s].dirty = true;
        dirty_list[ndirty++] = s;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata: superblock, group descriptors, inodes                     */
/* ------------------------------------------------------------------ */

static int flush_meta_(bool counts)
{
    if (counts && counts_dirty) {
        sb_dirty = gd_dirty = true;
        counts_dirty = false;
    }
    if (sb_dirty) {
        int s = bread(0);
        if (s < 0) return s;
        memcpy(slot_data(s) + 1024, sb_raw, sizeof(sb_raw));
        if (bwrite(s)) return -EIO;
        sb_dirty = false;
    }
    if (gd_dirty) {
        uint32_t nbytes = ngroups * sizeof(struct ext2_gd);
        for (uint32_t off = 0; off < nbytes; off += BS) {
            int s = bread(sb->s_first_data_block + 1 + off / BS);
            if (s < 0) return s;
            uint32_t n = nbytes - off < BS ? nbytes - off : BS;
            memcpy(slot_data(s), (uint8_t *)gd + off, n);
            if (bwrite(s)) return -EIO;
        }
        gd_dirty = false;
    }
    return flush_dirty();                 /* and every block the operation changed */
}

static int flush_meta(void) { return flush_meta_(false); }

static int inode_loc(uint32_t ino, uint64_t *blk, uint32_t *off)
{
    if (ino < 1 || ino > sb->s_inodes_count) return -EINVAL;
    uint32_t g = (ino - 1) / sb->s_inodes_per_group;
    uint64_t byte = (uint64_t)((ino - 1) % sb->s_inodes_per_group) * inode_size;
    *blk = gd[g].bg_inode_table + byte / BS;
    *off = byte % BS;
    return 0;
}

static int read_inode(uint32_t ino, struct ext2_inode *in)
{
    uint64_t blk;
    uint32_t off;
    if (inode_loc(ino, &blk, &off)) return -EINVAL;
    int s = bread(blk);
    if (s < 0) return s;
    memcpy(in, slot_data(s) + off, sizeof(*in));
    return 0;
}

static int write_inode(uint32_t ino, const struct ext2_inode *in)
{
    uint64_t blk;
    uint32_t off;
    if (inode_loc(ino, &blk, &off)) return -EINVAL;
    int s = bread(blk);
    if (s < 0) return s;
    memcpy(slot_data(s) + off, in, sizeof(*in));
    return bwrite(s);
}

static uint64_t inode_size_of(const struct ext2_inode *in)
{
    uint64_t size = in->i_size;
    if (S_ISREG(in->i_mode)) size |= (uint64_t)in->i_size_high << 32;
    return size;
}

static void set_inode_size(struct ext2_inode *in, uint64_t size)
{
    in->i_size = (uint32_t)size;
    if (S_ISREG(in->i_mode)) in->i_size_high = (uint32_t)(size >> 32);
}

/* ------------------------------------------------------------------ */
/* Block and inode allocation (per-group bitmaps)                      */
/* ------------------------------------------------------------------ */

/* The first free bit from *hint on (then from the start), set; *hint moves past it. Full bytes
 * are skipped whole: a big file used to rescan its group's bitmap bit by bit for every block
 * (quadratic), and starting where the last one was keeps a file's blocks contiguous */
static int64_t bitmap_alloc(uint64_t bitmap_blk, uint32_t nbits, uint32_t *hint)
{
    int s = bread(bitmap_blk);
    if (s < 0) return s;
    uint8_t *bm = slot_data(s);
    uint32_t start = *hint < nbits ? *hint : 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = pass ? 0 : start, to = pass ? start : nbits;
        for (uint32_t i = from; i < to; ) {
            if (!(i % 8) && bm[i / 8] == 0xff) { i += 8; continue; }
            if (!(bm[i / 8] & (1u << (i % 8)))) {
                bm[i / 8] |= 1u << (i % 8);
                if (bwrite(s)) return -EIO;
                *hint = i + 1;
                return i;
            }
            i++;
        }
    }
    return -ENOSPC;
}
static uint32_t block_hint[MAX_GROUPS], inode_hint[MAX_GROUPS];   /* bitmap_alloc's, per group */

/* Free (zero) bits among the first nbits of a bitmap block, or -errno */
static int64_t bitmap_count_free(uint64_t bitmap_blk, uint32_t nbits)
{
    int s = bread(bitmap_blk);
    if (s < 0) return s;
    uint8_t *bm = slot_data(s);
    int64_t n = 0;
    for (uint32_t i = 0; i < nbits; i++) n += !(bm[i / 8] & (1u << (i % 8)));
    return n;
}

/* Write-back: the blocks an operation changes stay dirty in the cache and go to the disk together
 * (flush_meta) when there are many of them, on FS_OP_SYNC (sync, fsync, every few seconds from
 * posix_server, unmount) and at once for an operation that frees blocks or inodes: a block freed
 * is never reused before its release is on the disk, so a cut cannot leave another file's new
 * data under a deleted file's inode. The order within a flush stays the same (fresh blocks
 * first) */
static bool op_freed;                  /* this operation freed something: flush it now */
static int flush_err;                  /* a deferred flush failed: the next FS_OP_SYNC says so */
#define DIRTY_MAX (nslots / 4)

static int bitmap_free(uint64_t bitmap_blk, uint32_t bit)
{
    op_freed = true;
    int s = bread(bitmap_blk);
    if (s < 0) return s;
    slot_data(s)[bit / 8] &= ~(1u << (bit % 8));
    return bwrite(s);
}

static int64_t alloc_block(uint32_t goal_group)
{
    if (!sb->s_free_blocks_count) return -ENOSPC;
    for (uint32_t i = 0; i < ngroups; i++) {
        uint32_t g = (goal_group + i) % ngroups;
        if (!gd[g].bg_free_blocks_count) continue;
        int64_t bit = bitmap_alloc(gd[g].bg_block_bitmap, sb->s_blocks_per_group, &block_hint[g]);
        if (bit == -ENOSPC) continue;
        if (bit < 0) return bit;
        gd[g].bg_free_blocks_count--;
        sb->s_free_blocks_count--;
        counts_dirty = true;
        return (int64_t)g * sb->s_blocks_per_group + bit + sb->s_first_data_block;
    }
    return -ENOSPC;
}

/* A new, zeroed block, so old data is never seen */
static int64_t alloc_zeroed_block(uint32_t goal_group)
{
    int64_t b = alloc_block(goal_group);
    if (b < 0) return b;
    int s = bzero_get(b);
    slots[s].fresh = true;                 /* on the disk before whatever points to it */
    if (bwrite(s)) return -EIO;
    return b;
}

static int free_block(uint32_t b)
{
    if (b < sb->s_first_data_block || b >= sb->s_blocks_count) return -EIO;
    uint32_t g = (b - sb->s_first_data_block) / sb->s_blocks_per_group;
    uint32_t bit = (b - sb->s_first_data_block) % sb->s_blocks_per_group;
    if (bitmap_free(gd[g].bg_block_bitmap, bit)) return -EIO;
    gd[g].bg_free_blocks_count++;
    sb->s_free_blocks_count++;
    counts_dirty = true;
    return 0;
}

static uint32_t inode_group(uint32_t ino) { return (ino - 1) / sb->s_inodes_per_group; }

static int64_t alloc_inode(uint32_t goal_group, bool is_dir)
{
    if (!sb->s_free_inodes_count) return -ENOSPC;
    for (uint32_t i = 0; i < ngroups; i++) {
        uint32_t g = (goal_group + i) % ngroups;
        if (!gd[g].bg_free_inodes_count) continue;
        int64_t bit = bitmap_alloc(gd[g].bg_inode_bitmap, sb->s_inodes_per_group, &inode_hint[g]);
        if (bit == -ENOSPC) continue;
        if (bit < 0) return bit;
        gd[g].bg_free_inodes_count--;
        if (is_dir) gd[g].bg_used_dirs_count++;
        sb->s_free_inodes_count--;
        counts_dirty = true;
        if (is_dir) gd_dirty = true;       /* recount_free does not recount directories */
        return (int64_t)g * sb->s_inodes_per_group + bit + 1;
    }
    return -ENOSPC;
}

static int free_inode(uint32_t ino, bool is_dir)
{
    uint32_t g = inode_group(ino);
    if (bitmap_free(gd[g].bg_inode_bitmap, (ino - 1) % sb->s_inodes_per_group)) return -EIO;
    gd[g].bg_free_inodes_count++;
    if (is_dir) gd[g].bg_used_dirs_count--;
    sb->s_free_inodes_count++;
    counts_dirty = true;
    if (is_dir) gd_dirty = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Block map: logical -> physical block (direct and indirect)          */
/* ------------------------------------------------------------------ */

/* Returns the physical block, 0 for a hole, or -errno. With create=true it
 * allocates (zeroed) any missing intermediate blocks and the data block. */
static int64_t bmap(uint32_t ino, struct ext2_inode *in, uint64_t lblk, bool create, bool *dirty)
{
    uint32_t idx[4];
    int depth;
    if (lblk < EXT2_NDIR_BLOCKS) {
        idx[0] = lblk; depth = 0;
    } else if ((lblk -= EXT2_NDIR_BLOCKS) < APB) {
        idx[0] = 12; idx[1] = lblk; depth = 1;
    } else if ((lblk -= APB) < (uint64_t)APB * APB) {
        idx[0] = 13; idx[1] = lblk / APB; idx[2] = lblk % APB; depth = 2;
    } else if ((lblk -= (uint64_t)APB * APB) < (uint64_t)APB * APB * APB) {
        idx[0] = 14; idx[1] = lblk / ((uint64_t)APB * APB); idx[2] = (lblk / APB) % APB; idx[3] = lblk % APB;
        depth = 3;
    } else {
        return -EFBIG;
    }

    uint32_t goal = inode_group(ino);
    uint32_t b = in->i_block[idx[0]];
    if (!b) {
        if (!create) return 0;
        int64_t nb = alloc_zeroed_block(goal);
        if (nb < 0) return nb;
        b = in->i_block[idx[0]] = nb;
        in->i_blocks += BS / 512;
        *dirty = true;
    }
    for (int d = 1; d <= depth; d++) {
        int s = bread(b);
        if (s < 0) return s;
        uint32_t next = ((uint32_t *)slot_data(s))[idx[d]];
        if (!next) {
            if (!create) return 0;
            int64_t nb = alloc_zeroed_block(goal);
            if (nb < 0) return nb;
            s = bread(b);                 /* the allocation may have recycled the slot */
            if (s < 0) return s;
            ((uint32_t *)slot_data(s))[idx[d]] = next = nb;
            if (bwrite(s)) return -EIO;
            in->i_blocks += BS / 512;
            *dirty = true;
        }
        b = next;
    }
    return b;
}

/* Frees a block tree: level 0 = data block, 1..3 = indirect */
static int free_tree(struct ext2_inode *in, uint32_t b, int level)
{
    if (level > 0) {
        for (uint32_t i = 0; i < APB; i++) {
            int s = bread(b);
            if (s < 0) return s;
            uint32_t e = ((uint32_t *)slot_data(s))[i];
            if (e && free_tree(in, e, level - 1)) return -EIO;
        }
    }
    in->i_blocks -= BS / 512;
    return free_block(b);
}

/* Frees, inside indirect block b of the given level, everything from relative
 * logical block first on. Returns 1 if the indirect block ended up empty, 0 if not, or -errno. */
static int trunc_ind(struct ext2_inode *in, uint32_t b, int level, uint64_t first)
{
    uint64_t span = 1;
    for (int i = 1; i < level; i++) span *= APB;
    bool empty = true;
    for (uint32_t i = 0; i < APB; i++) {
        int s = bread(b);
        if (s < 0) return s;
        uint32_t e = ((uint32_t *)slot_data(s))[i];
        if (!e) continue;
        uint64_t start = i * span;
        bool clear = false;
        if (start >= first) {
            if (free_tree(in, e, level - 1)) return -EIO;
            clear = true;
        } else if (level > 1 && start + span > first) {
            int r = trunc_ind(in, e, level - 1, first - start);
            if (r < 0) return r;
            if (r) {
                in->i_blocks -= BS / 512;
                if (free_block(e)) return -EIO;
                clear = true;
            }
        }
        if (clear) {
            s = bread(b);
            if (s < 0) return s;
            ((uint32_t *)slot_data(s))[i] = 0;
            if (bwrite(s)) return -EIO;
        } else {
            empty = false;
        }
    }
    return empty;
}

static int truncate_blocks(struct ext2_inode *in, uint64_t new_size)
{
    uint64_t first = (new_size + BS - 1) / BS;
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (i >= first && in->i_block[i]) {
            in->i_blocks -= BS / 512;
            if (free_block(in->i_block[i])) return -EIO;
            in->i_block[i] = 0;
        }
    }
    uint64_t base = EXT2_NDIR_BLOCKS, span = APB;
    for (int level = 1; level <= 3; level++) {
        uint32_t *slot = &in->i_block[11 + level];
        if (*slot) {
            uint64_t rel = first > base ? first - base : 0;
            if (rel == 0) {
                if (free_tree(in, *slot, level)) return -EIO;
                *slot = 0;
            } else if (rel < span) {
                int r = trunc_ind(in, *slot, level, rel);
                if (r < 0) return r;
                if (r) {
                    in->i_blocks -= BS / 512;
                    if (free_block(*slot)) return -EIO;
                    *slot = 0;
                }
            }
        }
        base += span;
        span *= APB;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reading and writing contents                                        */
/* ------------------------------------------------------------------ */

/* Sequential reads: the last file read and where it stopped, for read-ahead */
/* Read-ahead, as Linux does it: a window that grows while a file is read sequentially (128 KiB,
 * doubling up to 2 MiB), fetched ahead when what is already read runs low; any other access
 * starts it small again. It used to be a fixed 128 KiB recomputed on every call, which in
 * practice asked the disk for 64 KiB at a time */
static uint32_t ra_ino;
static uint64_t ra_next;               /* byte offset a sequential read continues at */
static uint64_t ra_end;                /* first block not fetched ahead yet */
static uint32_t ra_win = 32;           /* blocks */
#define RA_MIN 32                      /* 128 KiB */
#define RA_MAX 512                     /* 2 MiB */

static long file_read(uint32_t ino, struct ext2_inode *in, uint64_t off, uint32_t len, uint8_t *out)
{
    uint64_t size = inode_size_of(in);
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    /* The blocks this read needs (and, if it continues the previous one, the next READ_AHEAD)
     * that are not cached are read together first */
    {
        static uint32_t pbs[STAGE_BLOCKS * 4];
        uint64_t first = off / BS, last = (off + len - 1) / BS + 1;     /* blocks [first, last) */
        uint64_t fblocks = (size + BS - 1) / BS;
        bool seq = ino == ra_ino && off == ra_next;
        if (!seq) { ra_win = RA_MIN; ra_end = last; }
        uint64_t to = last;
        if ((seq || off == 0) && last + ra_win / 2 > ra_end) {         /* the window runs low */
            to = (ra_end > last ? ra_end : last) + ra_win;
            if (seq && ra_win < RA_MAX) ra_win *= 2;
        }
        if (to > fblocks) to = fblocks;
        if (to > ra_end) ra_end = to;
        if (to - first > sizeof pbs / sizeof *pbs) to = first + sizeof pbs / sizeof *pbs;
        uint32_t k = 0;
        for (uint64_t b = first; b < to; b++) {
            int64_t pb = bmap(ino, in, b, false, NULL);
            if (pb < 0) break;
            pbs[k++] = pb;
        }
        bread_many(pbs, k);            /* the cached ones are skipped; errors show up below */
    }
    ra_ino = ino;
    ra_next = off + len;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bo = pos % BS;
        uint32_t n = BS - bo < len - done ? BS - bo : len - done;
        int64_t pb = bmap(ino, in, pos / BS, false, NULL);
        if (pb < 0) return done ? (long)done : pb;
        if (pb == 0) {
            memset(out + done, 0, n);           /* hole: zeros */
        } else {
            int s = bread(pb);
            if (s < 0) return done ? (long)done : s;
            memcpy(out + done, slot_data(s) + bo, n);
        }
        done += n;
    }
    return done;
}

static long file_write(uint32_t ino, struct ext2_inode *in, uint64_t off, uint32_t len, const uint8_t *data)
{
    bool dirty = false;
    uint32_t done = 0;
    long err = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bo = pos % BS;
        uint32_t n = BS - bo < len - done ? BS - bo : len - done;
        int64_t pb = bmap(ino, in, pos / BS, true, &dirty);
        if (pb < 0) { err = pb; break; }
        int s = n == BS ? bget(pb) : bread(pb);         /* a whole block: no need to read it */
        if (s < 0) { err = s; break; }
        memcpy(slot_data(s) + bo, data + done, n);
        if (bwrite(s)) { err = -EIO; break; }
        done += n;
    }
    if (done) {
        if (off + done > inode_size_of(in)) set_inode_size(in, off + done);
        in->i_mtime = in->i_ctime = now();
        dirty = true;
    }
    if (dirty && write_inode(ino, in)) return -EIO;
    return done ? (long)done : err;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static uint32_t rec_len_for(uint32_t name_len) { return (8 + name_len + 3) & ~3u; }

static uint8_t ftype_of_mode(uint16_t mode)
{
    if (S_ISREG(mode)) return FT_REG;
    if (S_ISDIR(mode)) return FT_DIR;
    if (S_ISLNK(mode)) return FT_SYMLINK;
    if ((mode & S_IFMT) == 0010000) return FT_FIFO;             /* named pipe (mkfifo) */
    if ((mode & S_IFMT) == 0140000) return 6;                   /* socket (AF_UNIX bind) */
    return FT_UNKNOWN;
}

static bool dirent_ok(const struct ext2_dirent *de, uint32_t off)
{
    return de->rec_len >= 8 && !(de->rec_len & 3) && off + de->rec_len <= BS && de->name_len + 8u <= de->rec_len;
}

/* Looks name up in the directory. Returns the inode (0 if absent) or -errno. */
static int64_t dir_lookup(uint32_t dir, const char *name, uint32_t len)
{
    struct ext2_inode in;
    if (read_inode(dir, &in)) return -EIO;
    if (!S_ISDIR(in.i_mode)) return -ENOTDIR;
    uint64_t nblocks = in.i_size / BS;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        int64_t pb = bmap(dir, &in, lb, false, NULL);
        if (pb <= 0) continue;
        int s = bread(pb);
        if (s < 0) return s;
        for (uint32_t off = 0; off < BS;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(slot_data(s) + off);
            if (!dirent_ok(de, off)) break;
            if (de->inode && de->name_len == len && !memcmp(de->name, name, len)) return de->inode;
            off += de->rec_len;
        }
    }
    return 0;
}

static int dir_add(uint32_t dir, const char *name, uint32_t len, uint32_t ino, uint8_t ftype)
{
    struct ext2_inode in;
    if (read_inode(dir, &in)) return -EIO;
    bool dirty = false;
    if (in.i_flags & EXT2_INDEX_FL) {          /* the htree index would no longer be valid */
        in.i_flags &= ~EXT2_INDEX_FL;
        dirty = true;
    }
    uint32_t need = rec_len_for(len);
    uint64_t nblocks = in.i_size / BS;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        int64_t pb = bmap(dir, &in, lb, false, NULL);
        if (pb <= 0) continue;
        int s = bread(pb);
        if (s < 0) return s;
        for (uint32_t off = 0; off < BS;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(slot_data(s) + off);
            if (!dirent_ok(de, off)) break;
            uint32_t used = de->inode ? rec_len_for(de->name_len) : 0;
            if (de->rec_len - used >= need) {
                struct ext2_dirent *nd = de;
                if (used) {                     /* split the existing entry */
                    nd = (struct ext2_dirent *)((uint8_t *)de + used);
                    nd->rec_len = de->rec_len - used;
                    de->rec_len = used;
                }
                nd->inode = ino;
                nd->name_len = len;
                nd->file_type = ftype;
                memcpy(nd->name, name, len);
                if (bwrite(s)) return -EIO;
                in.i_mtime = in.i_ctime = now();
                return write_inode(dir, &in);
            }
            off += de->rec_len;
        }
    }
    /* No room: a new block at the end of the directory */
    int64_t pb = bmap(dir, &in, nblocks, true, &dirty);
    if (pb < 0) return pb;
    int s = bread(pb);
    if (s < 0) return s;
    struct ext2_dirent *de = (struct ext2_dirent *)slot_data(s);
    de->inode = ino;
    de->rec_len = BS;
    de->name_len = len;
    de->file_type = ftype;
    memcpy(de->name, name, len);
    if (bwrite(s)) return -EIO;
    in.i_size += BS;
    in.i_mtime = in.i_ctime = now();
    return write_inode(dir, &in);
}

static int dir_remove(uint32_t dir, const char *name, uint32_t len)
{
    struct ext2_inode in;
    if (read_inode(dir, &in)) return -EIO;
    if (in.i_flags & EXT2_INDEX_FL) in.i_flags &= ~EXT2_INDEX_FL;
    uint64_t nblocks = in.i_size / BS;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        int64_t pb = bmap(dir, &in, lb, false, NULL);
        if (pb <= 0) continue;
        int s = bread(pb);
        if (s < 0) return s;
        struct ext2_dirent *prev = NULL;
        for (uint32_t off = 0; off < BS;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(slot_data(s) + off);
            if (!dirent_ok(de, off)) break;
            if (de->inode && de->name_len == len && !memcmp(de->name, name, len)) {
                if (prev) prev->rec_len += de->rec_len;    /* merged into the previous one */
                else de->inode = 0;                        /* first in the block: emptied */
                if (bwrite(s)) return -EIO;
                in.i_mtime = in.i_ctime = now();
                return write_inode(dir, &in);
            }
            prev = de;
            off += de->rec_len;
        }
    }
    return -ENOENT;
}

static int64_t dir_is_empty(uint32_t dir)
{
    struct ext2_inode in;
    if (read_inode(dir, &in)) return -EIO;
    uint64_t nblocks = in.i_size / BS;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        int64_t pb = bmap(dir, &in, lb, false, NULL);
        if (pb <= 0) continue;
        int s = bread(pb);
        if (s < 0) return s;
        for (uint32_t off = 0; off < BS;) {
            struct ext2_dirent *de = (struct ext2_dirent *)(slot_data(s) + off);
            if (!dirent_ok(de, off)) break;
            bool dot = (de->name_len == 1 && de->name[0] == '.') ||
                       (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
            if (de->inode && !dot) return 0;
            off += de->rec_len;
        }
    }
    return 1;
}

/* Changes ".." of a moved directory (the second entry of the first block) */
static int set_dotdot(uint32_t dir, uint32_t parent)
{
    struct ext2_inode in;
    if (read_inode(dir, &in)) return -EIO;
    int64_t pb = bmap(dir, &in, 0, false, NULL);
    if (pb <= 0) return -EIO;
    int s = bread(pb);
    if (s < 0) return s;
    struct ext2_dirent *dot = (struct ext2_dirent *)slot_data(s);
    if (!dirent_ok(dot, 0)) return -EIO;
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(slot_data(s) + dot->rec_len);
    dotdot->inode = parent;
    return bwrite(s);
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */

static long read_link(uint32_t ino, struct ext2_inode *in, char *out, uint32_t max)
{
    uint32_t len = in->i_size;
    if (len > max) len = max;
    uint32_t ea_blocks = in->i_file_acl ? BS / 512 : 0;
    if (in->i_blocks - ea_blocks == 0) {        /* "fast" link: stored in i_block */
        memcpy(out, in->i_block, len);
        return len;
    }
    return file_read(ino, in, 0, len, (uint8_t *)out);
}

/* Credentials of the caller (FS_OP_SETCRED): the search (x) permission of every directory on
 * a path is checked against them, as a Unix kernel does while it walks the path */
static uint32_t cred_uid, cred_gid, cred_ngroups, cred_groups[32];

static bool may_search(uint32_t dir_ino)
{
    if (cred_uid == 0) return true;
    struct ext2_inode d;
    if (read_inode(dir_ino, &d)) return false;
    uint32_t uid = d.i_uid | (uint32_t)d.i_uid_high << 16, gid = d.i_gid | (uint32_t)d.i_gid_high << 16;
    uint32_t bits = d.i_mode;
    if (uid == cred_uid) return bits & 0100;
    bool member = gid == cred_gid;
    for (uint32_t i = 0; i < cred_ngroups && !member; i++) member = cred_groups[i] == gid;
    return member ? bits & 0010 : bits & 0001;
}

/* Resolves an absolute path. follow_last: follow the last symbolic link. */
static int64_t resolve(const char *path, bool follow_last)
{
    static char work[FS_PATH_MAX * 2], tmp[FS_PATH_MAX * 2];
    size_t plen = strlen(path);
    if (plen >= FS_PATH_MAX) return -ENAMETOOLONG;
    memcpy(work, path, plen + 1);
    uint32_t cur = EXT2_ROOT_INO;
    const char *p = work;
    int links = 0;

    for (;;) {
        while (*p == '/') p++;
        if (!*p) return cur;
        const char *end = p;
        while (*end && *end != '/') end++;
        uint32_t len = end - p;
        if (len > 255) return -ENAMETOOLONG;
        if (!may_search(cur)) return -EACCES;
        int64_t next = dir_lookup(cur, p, len);
        if (next < 0) return next;
        if (!next) return -ENOENT;

        struct ext2_inode in;
        if (read_inode(next, &in)) return -EIO;
        bool last = true;
        for (const char *q = end; *q; q++) if (*q != '/') { last = false; break; }
        if (S_ISLNK(in.i_mode) && (!last || follow_last)) {
            if (++links > 8) return -ELOOP;
            char target[FS_PATH_MAX];
            long tl = read_link(next, &in, target, FS_PATH_MAX - 1);
            if (tl < 0) return tl;
            size_t rest = strlen(end);
            if (tl + rest + 2 > sizeof(tmp)) return -ENAMETOOLONG;
            memcpy(tmp, target, tl);
            tmp[tl] = '/';
            memcpy(tmp + tl + 1, end, rest + 1);
            memcpy(work, tmp, tl + rest + 2);
            p = work;
            if (target[0] == '/') cur = EXT2_ROOT_INO;   /* absolute: from the root */
            continue;                                    /* relative: from the current dir */
        }
        cur = next;
        p = end;
    }
}

/* Splits "/a/b/c" into the inode of "/a/b" and the name "c" */
static int64_t resolve_parent(const char *path, const char **name, uint32_t *name_len)
{
    static char dir[FS_PATH_MAX];
    size_t len = strlen(path);
    if (len >= FS_PATH_MAX) return -ENAMETOOLONG;
    while (len > 1 && path[len - 1] == '/') len--;
    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    if (slash == len) return -EINVAL;                 /* "/" has no parent */
    *name = path + slash;
    *name_len = len - slash;
    if (*name_len > 255) return -ENAMETOOLONG;
    if ((*name_len == 1 && (*name)[0] == '.') || (*name_len == 2 && (*name)[0] == '.' && (*name)[1] == '.'))
        return -EINVAL;
    memcpy(dir, path, slash);
    dir[slash ? slash : 1] = 0;
    if (!slash) dir[0] = '/';
    int64_t parent = resolve(dir, true);
    if (parent < 0) return parent;
    struct ext2_inode in;
    if (read_inode(parent, &in)) return -EIO;
    if (!S_ISDIR(in.i_mode)) return -ENOTDIR;
    if (!may_search(parent)) return -EACCES;
    return parent;
}

/* ------------------------------------------------------------------ */
/* Protocol operations                                                 */
/* ------------------------------------------------------------------ */

/* Linux x86_64 struct stat (144 bytes) */
struct k_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    int64_t  st_atime, st_atime_ns, st_mtime, st_mtime_ns, st_ctime, st_ctime_ns;
    int64_t  unused[3];
};
_Static_assert(sizeof(struct k_stat) == 144, "k_stat");

static long op_stat(uint32_t ino)
{
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    struct k_stat *st = (struct k_stat *)fs_buf;
    memset(st, 0, sizeof(*st));
    st->st_dev = 0x0801;                 /* "sda1" */
    st->st_ino = ino;
    st->st_nlink = in.i_links_count;
    st->st_mode = in.i_mode;
    st->st_uid = in.i_uid | ((uint32_t)in.i_uid_high << 16);
    st->st_gid = in.i_gid | ((uint32_t)in.i_gid_high << 16);
    st->st_size = inode_size_of(&in);
    st->st_blksize = BS;
    st->st_blocks = in.i_blocks;
    st->st_atime = in.i_atime;
    st->st_mtime = in.i_mtime;
    st->st_ctime = in.i_ctime;
    return 0;
}

static long op_getdents(uint32_t ino, uint64_t pos, uint32_t max)
{
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    if (!S_ISDIR(in.i_mode)) return -ENOTDIR;
    if (max > FS_BUF_SIZE) max = FS_BUF_SIZE;
    uint8_t *out = (uint8_t *)fs_buf;
    uint32_t used = 0;
    while (pos < in.i_size) {
        int64_t pb = bmap(ino, &in, pos / BS, false, NULL);
        if (pb < 0) return pb;
        if (pb == 0) { pos = (pos / BS + 1) * BS; continue; }
        int s = bread(pb);
        if (s < 0) return s;
        uint32_t off = pos % BS;
        struct ext2_dirent *de = (struct ext2_dirent *)(slot_data(s) + off);
        if (!dirent_ok(de, off)) { pos = (pos / BS + 1) * BS; continue; }
        uint64_t next = pos + de->rec_len;
        if (de->inode) {
            /* struct linux_dirent64: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[] */
            uint32_t reclen = (19 + de->name_len + 1 + 7) & ~7u;
            if (used + reclen > max) break;
            static const uint8_t dt[8] = { 0, 8, 4, 2, 6, 1, 12, 10 };
            uint8_t *r = out + used;
            *(uint64_t *)r = de->inode;
            *(int64_t *)(r + 8) = next;
            *(uint16_t *)(r + 16) = reclen;
            r[18] = dt[de->file_type & 7];
            memcpy(r + 19, de->name, de->name_len);
            memset(r + 19 + de->name_len, 0, reclen - 19 - de->name_len);
            used += reclen;
        }
        pos = next;
    }
    if (!used && pos < in.i_size) return -EINVAL;   /* the buffer is too small for even one entry */
    microkit_mr_set(1, pos);
    return used;
}

static long op_create(const char *path, uint32_t mode, uint32_t uid, uint32_t gid)
{
    const char *name;
    uint32_t nlen;
    int64_t parent = resolve_parent(path, &name, &nlen);
    if (parent < 0) return parent;
    int64_t exists = dir_lookup(parent, name, nlen);
    if (exists < 0) return exists;
    if (exists) return -EEXIST;
    bool is_dir = S_ISDIR(mode);

    struct ext2_inode pin;
    if (read_inode(parent, &pin)) return -EIO;
    if (is_dir && pin.i_links_count >= 32000) return -EMLINK;

    int64_t ino = alloc_inode(inode_group(parent), is_dir);
    if (ino < 0) return ino;
    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = mode;
    in.i_uid = uid & 0xffff; in.i_uid_high = uid >> 16;            /* the creator (posix_server says who) */
    in.i_gid = gid & 0xffff; in.i_gid_high = gid >> 16;
    in.i_links_count = is_dir ? 2 : 1;
    in.i_atime = in.i_ctime = in.i_mtime = now();
    if (is_dir) {
        int64_t b = alloc_zeroed_block(inode_group(ino));
        if (b < 0) { free_inode(ino, true); return b; }
        int s = bread(b);
        if (s < 0) return s;
        struct ext2_dirent *dot = (struct ext2_dirent *)slot_data(s);
        dot->inode = ino; dot->rec_len = 12; dot->name_len = 1; dot->file_type = FT_DIR;
        dot->name[0] = '.';
        struct ext2_dirent *dotdot = (struct ext2_dirent *)(slot_data(s) + 12);
        dotdot->inode = parent; dotdot->rec_len = BS - 12; dotdot->name_len = 2; dotdot->file_type = FT_DIR;
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        if (bwrite(s)) return -EIO;
        in.i_block[0] = b;
        in.i_blocks = BS / 512;
        in.i_size = BS;
    }
    if (write_inode(ino, &in)) return -EIO;
    int r = dir_add(parent, name, nlen, ino, ftype_of_mode(mode));
    if (r) return r;
    if (is_dir) {                                  /* the child's ".." links to the parent */
        if (read_inode(parent, &pin)) return -EIO;
        pin.i_links_count++;
        if (write_inode(parent, &pin)) return -EIO;
    }
    return ino;
}

static int release_inode(uint32_t ino, struct ext2_inode *in)
{
    bool is_dir = S_ISDIR(in->i_mode);
    if (!S_ISLNK(in->i_mode) || in->i_blocks) {
        if (truncate_blocks(in, 0)) return -EIO;
    }
    in->i_links_count = 0;
    in->i_size = 0;
    in->i_dtime = now();
    if (write_inode(ino, in)) return -EIO;
    return free_inode(ino, is_dir);
}

static long op_unlink(const char *path, bool rmdir)
{
    const char *name;
    uint32_t nlen;
    int64_t parent = resolve_parent(path, &name, &nlen);
    if (parent < 0) return parent;
    int64_t ino = dir_lookup(parent, name, nlen);
    if (ino < 0) return ino;
    if (!ino) return -ENOENT;
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    if (rmdir) {
        if (!S_ISDIR(in.i_mode)) return -ENOTDIR;
        int64_t empty = dir_is_empty(ino);
        if (empty < 0) return empty;
        if (!empty) return -ENOTEMPTY;
    } else if (S_ISDIR(in.i_mode)) {
        return -EISDIR;
    }
    int r = dir_remove(parent, name, nlen);
    if (r) return r;
    if (rmdir) {
        struct ext2_inode pin;
        if (read_inode(parent, &pin)) return -EIO;
        pin.i_links_count--;
        if (write_inode(parent, &pin)) return -EIO;
        return release_inode(ino, &in);
    }
    in.i_links_count--;
    in.i_ctime = now();
    if (in.i_links_count == 0) return release_inode(ino, &in);
    return write_inode(ino, &in);
}

/* Is dir equal to ancestor or inside it? (so a dir is never moved into itself) */
static int64_t is_within(uint32_t dir, uint32_t ancestor)
{
    for (int depth = 0; depth < 4096; depth++) {
        if (dir == ancestor) return 1;
        if (dir == EXT2_ROOT_INO) return 0;
        int64_t up = dir_lookup(dir, "..", 2);
        if (up <= 0) return up < 0 ? up : -EIO;
        dir = up;
    }
    return -ELOOP;
}

static long op_rename(const char *from, const char *to)
{
    const char *oname, *nname;
    uint32_t olen, nlen;
    int64_t op = resolve_parent(from, &oname, &olen);
    if (op < 0) return op;
    int64_t ino = dir_lookup(op, oname, olen);
    if (ino < 0) return ino;
    if (!ino) return -ENOENT;
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    bool is_dir = S_ISDIR(in.i_mode);

    /* resolve_parent uses a static buffer: save the old name */
    char oname_copy[256];
    memcpy(oname_copy, oname, olen);

    int64_t np = resolve_parent(to, &nname, &nlen);
    if (np < 0) return np;
    if (is_dir) {
        int64_t inside = is_within(np, ino);
        if (inside < 0) return inside;
        if (inside) return -EINVAL;
    }
    int64_t target = dir_lookup(np, nname, nlen);
    if (target < 0) return target;
    if (target == ino) return 0;
    if (target) {
        struct ext2_inode tin;
        if (read_inode(target, &tin)) return -EIO;
        if (S_ISDIR(tin.i_mode)) {
            if (!is_dir) return -EISDIR;
            int64_t empty = dir_is_empty(target);
            if (empty < 0) return empty;
            if (!empty) return -ENOTEMPTY;
        } else if (is_dir) {
            return -ENOTDIR;
        }
        int r = dir_remove(np, nname, nlen);
        if (r) return r;
        if (S_ISDIR(tin.i_mode)) {
            struct ext2_inode pin;
            if (read_inode(np, &pin)) return -EIO;
            pin.i_links_count--;
            if (write_inode(np, &pin)) return -EIO;
            if (release_inode(target, &tin)) return -EIO;
        } else if (--tin.i_links_count == 0) {
            if (release_inode(target, &tin)) return -EIO;
        } else if (write_inode(target, &tin)) {
            return -EIO;
        }
    }
    int r = dir_add(np, nname, nlen, ino, ftype_of_mode(in.i_mode));
    if (r) return r;
    r = dir_remove(op, oname_copy, olen);
    if (r) return r;
    if (is_dir && op != np) {
        if (set_dotdot(ino, np)) return -EIO;
        struct ext2_inode pin;
        if (read_inode(op, &pin)) return -EIO;
        pin.i_links_count--;
        if (write_inode(op, &pin)) return -EIO;
        if (read_inode(np, &pin)) return -EIO;
        pin.i_links_count++;
        if (write_inode(np, &pin)) return -EIO;
    }
    if (read_inode(ino, &in)) return -EIO;
    in.i_ctime = now();
    return write_inode(ino, &in);
}

static long op_symlink(const char *target, const char *path, uint32_t uid, uint32_t gid)
{
    size_t tlen = strlen(target);
    if (tlen >= BS) return -ENAMETOOLONG;
    int64_t ino = op_create(path, S_IFLNK | 0777, uid, gid);
    if (ino < 0) return ino;
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    if (tlen < sizeof(in.i_block)) {               /* fast link: fits in i_block */
        memcpy(in.i_block, target, tlen);
        in.i_size = tlen;
        return write_inode(ino, &in);
    }
    long w = file_write(ino, &in, 0, tlen, (const uint8_t *)target);
    return w < 0 ? w : 0;
}

/* Hard link: another name for the same inode (not for directories, as in Linux) */
static long op_link(const char *from, const char *to)
{
    int64_t ino = resolve(from, false);
    if (ino < 0) return ino;
    struct ext2_inode in;
    if (read_inode(ino, &in)) return -EIO;
    if (S_ISDIR(in.i_mode)) return -EPERM;
    if (in.i_links_count >= 65000) return -EMLINK;
    const char *name;
    uint32_t nlen;
    int64_t dir = resolve_parent(to, &name, &nlen);
    if (dir < 0) return dir;
    if (dir_lookup(dir, name, nlen) > 0) return -EEXIST;
    int r = dir_add(dir, name, nlen, ino, ftype_of_mode(in.i_mode));
    if (r) return r;
    in.i_links_count++;
    in.i_ctime = now();
    return write_inode(ino, &in);
}

/* Linux x86_64 struct statfs (120 bytes) */
static long op_statfs(void)
{
    int64_t *f = (int64_t *)fs_buf;
    memset(f, 0, 120);
    f[0] = EXT2_MAGIC;                              /* f_type */
    f[1] = BS;                                      /* f_bsize */
    f[2] = sb->s_blocks_count;                      /* f_blocks */
    f[3] = sb->s_free_blocks_count;                 /* f_bfree */
    f[4] = sb->s_free_blocks_count > sb->s_r_blocks_count
               ? sb->s_free_blocks_count - sb->s_r_blocks_count : 0;   /* f_bavail */
    f[5] = sb->s_inodes_count;                      /* f_files */
    f[6] = sb->s_free_inodes_count;                 /* f_ffree */
    f[8] = 255;                                     /* f_namelen */
    f[9] = BS;                                      /* f_frsize */
    f[10] = read_only ? 1 : 0;                      /* f_flags: ST_RDONLY */
    return 0;
}

static long op_sync(bool unmount);
static int load_meta(void);
static int recount_free(void);
#define EXT2_VALID_FS 1                /* s_state: cleanly unmounted */

/* FS_OP_RAW: the partition as bytes, through the block cache (what e2fsck writes is what the
 * file system then reads). Writes only while read-only: nothing of ours is in flight then */
static long op_raw(uint64_t what, uint64_t off, uint64_t len, uint8_t *buf)
{
    uint64_t size = disk_blocks * BS;
    if (what == FS_RAW_SIZE) return (long)size;
    if (what > FS_RAW_WRITE || len > FS_BUF_SIZE) return -EINVAL;
    if (what == FS_RAW_WRITE && !read_only) return -EBUSY;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    if (what == FS_RAW_READ) {              /* the blocks it spans, read together */
        uint32_t v[FS_BUF_SIZE / BS + 1], n = 0;
        for (uint64_t b = off / BS; b <= (off + len - 1) / BS; b++) v[n++] = b;
        if (bread_many(v, n)) return -EIO;
    }
    uint64_t done = 0;
    while (done < len) {
        uint64_t blk = (off + done) / BS;
        uint32_t in = (off + done) % BS;
        uint32_t n = BS - in < len - done ? BS - in : (uint32_t)(len - done);
        int s = what == FS_RAW_WRITE && in == 0 && n == BS ? bget(blk) : bread(blk);
        if (s < 0) return -EIO;
        if (what == FS_RAW_READ) {
            memcpy(buf + done, slot_data(s) + in, n);
        } else {
            memcpy(slot_data(s) + in, buf + done, n);
            bwrite(s);                      /* written out after the operation (flush_meta) */
        }
        done += n;
    }
    return (long)done;
}

/* FS_OP_REMOUNT */
static long op_remount(bool rw)
{
    if (!rw) return read_only ? 0 : op_sync(true);
    if (!read_only) return 0;
    if (ro_features) return -EROFS;
    if (flush_dirty() || load_meta()) return -EIO;     /* e2fsck may have changed them */
    if (!(sb->s_state & EXT2_VALID_FS)) return -EUCLEAN;    /* e2fsck did not finish the job */
    fs_dirty = false;
    read_only = false;
    sb->s_state &= ~EXT2_VALID_FS;            /* in use: clean again only after FS_OP_SYNC(1) */
    sb_dirty = true;
    microkit_dbg_puts("fs_ext2: mounted read-write\n");
    return 0;
}

static size_t put_ctr(char *b, size_t n, size_t max, const char *name, uint64_t cnt, uint64_t cyc)
{
    char line[96], num[24];
    size_t l = 0;
    for (const char *p = name; *p && l < 60; p++) line[l++] = *p;
    uint64_t v[2] = { cnt, cyc };
    for (int k = 0; k < 2; k++) {
        int i = 0;
        uint64_t x = v[k];
        do { num[i++] = '0' + x % 10; x /= 10; } while (x);
        line[l++] = ' ';
        while (i) line[l++] = num[--i];
    }
    line[l++] = '\n';
    if (n + l > max) return n;
    memcpy(b + n, line, l);
    return n + l;
}

static long op_stats(char *b, size_t max)
{
    static const char *names[32] = {
        [FS_OP_LOOKUP] = "lookup", [FS_OP_STAT] = "stat", [FS_OP_READ] = "read",
        [FS_OP_WRITE] = "write", [FS_OP_TRUNCATE] = "truncate", [FS_OP_GETDENTS] = "getdents",
        [FS_OP_READLINK] = "readlink", [FS_OP_CREATE] = "create", [FS_OP_UNLINK] = "unlink",
        [FS_OP_RENAME] = "rename", [FS_OP_SYMLINK] = "symlink", [FS_OP_CHMOD] = "chmod",
        [FS_OP_STATFS] = "statfs", [FS_OP_SETCLOCK] = "setclock", [FS_OP_SETTIMES] = "settimes",
        [FS_OP_CHOWN] = "chown", [FS_OP_LINK] = "link", [FS_OP_SYNC] = "sync",
        [FS_OP_SETCRED] = "setcred",
    };
    char name[48];
    size_t n = 0;
    for (int i = 0; i < 32; i++) {
        if (!st_op[i].n) continue;
        const char *nm = names[i] ? names[i] : "other";
        size_t l = 0;
        for (const char *p = "ext2.op."; *p; p++) name[l++] = *p;
        for (const char *p = nm; *p; p++) name[l++] = *p;
        name[l] = 0;
        n = put_ctr(b, n, max, name, st_op[i].n, st_op[i].cyc);
    }
    n = put_ctr(b, n, max, "ext2.flush_meta", st_flush.n, st_flush.cyc);
    n = put_ctr(b, n, max, "ext2.cache_lookup", st_cache_lookup.n, st_cache_lookup.cyc);
    n = put_ctr(b, n, max, "ext2.cache_hit", st_hits, 0);
    n = put_ctr(b, n, max, "ext2.cache_miss", st_misses, 0);
    n = put_ctr(b, n, max, "ext2.disk_read", st_disk_read.n, st_disk_read.cyc);
    n = put_ctr(b, n, max, "ext2.disk_write", st_disk_write.n, st_disk_write.cyc);
    n = put_ctr(b, n, max, "ext2.cache_slots", nslots, 0);
    return n;
}

static bool is_write_op(long op)
{
    return op == FS_OP_LINK || op == FS_OP_WRITE || op == FS_OP_TRUNCATE || op == FS_OP_CREATE || op == FS_OP_UNLINK ||
           op == FS_OP_RENAME || op == FS_OP_SYMLINK || op == FS_OP_CHMOD || op == FS_OP_SETTIMES || op == FS_OP_CHOWN;
}

static long dispatch(long op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    char *buf = (char *)fs_buf;
    struct ext2_inode in;

    switch (op) {
    case FS_OP_LOOKUP: {
        buf[FS_PATH_MAX - 1] = 0;
        int64_t ino = resolve(buf, a1);
        if (ino < 0) return ino;
        if (read_inode(ino, &in)) return -EIO;
        microkit_mr_set(1, in.i_mode);
        microkit_mr_set(2, inode_size_of(&in));
        microkit_mr_set(3, in.i_uid | (uint32_t)in.i_uid_high << 16);
        microkit_mr_set(4, in.i_gid | (uint32_t)in.i_gid_high << 16);
        return ino;
    }
    case FS_OP_STAT:
        return op_stat(a1);
    case FS_OP_READ:
        if (read_inode(a1, &in)) return -EIO;
        if (S_ISDIR(in.i_mode)) return -EISDIR;
        return file_read(a1, &in, a2, a3 > FS_BUF_SIZE ? FS_BUF_SIZE : a3, (uint8_t *)buf);
    case FS_OP_WRITE:
        if (read_inode(a1, &in)) return -EIO;
        if (S_ISDIR(in.i_mode)) return -EISDIR;
        return file_write(a1, &in, a2, a3 > FS_BUF_SIZE ? FS_BUF_SIZE : a3, (const uint8_t *)buf);
    case FS_OP_TRUNCATE: {
        if (read_inode(a1, &in)) return -EIO;
        if (!S_ISREG(in.i_mode)) return -EINVAL;
        if (a2 < inode_size_of(&in) && truncate_blocks(&in, a2)) return -EIO;
        set_inode_size(&in, a2);
        in.i_mtime = in.i_ctime = now();
        return write_inode(a1, &in);
    }
    case FS_OP_GETDENTS:
        return op_getdents(a1, a2, a3);
    case FS_OP_READLINK:
        if (read_inode(a1, &in)) return -EIO;
        if (!S_ISLNK(in.i_mode)) return -EINVAL;
        return read_link(a1, &in, buf, FS_BUF_SIZE);
    case FS_OP_CREATE:
        buf[FS_PATH_MAX - 1] = 0;
        return op_create(buf, a1, a2, a3);
    case FS_OP_UNLINK:
        buf[FS_PATH_MAX - 1] = 0;
        return op_unlink(buf, a1);
    case FS_OP_RENAME:
        buf[FS_PATH_MAX - 1] = 0;
        buf[FS_BUF_PATH2 + FS_PATH_MAX - 1] = 0;
        return op_rename(buf, buf + FS_BUF_PATH2);
    case FS_OP_SYMLINK:
        buf[FS_PATH_MAX - 1] = 0;
        buf[FS_BUF_PATH2 + FS_PATH_MAX - 1] = 0;
        return op_symlink(buf, buf + FS_BUF_PATH2, a1, a2);
    case FS_OP_LINK:
        buf[FS_PATH_MAX - 1] = 0;
        buf[FS_BUF_PATH2 + FS_PATH_MAX - 1] = 0;
        return op_link(buf, buf + FS_BUF_PATH2);
    case FS_OP_CHMOD:
        if (read_inode(a1, &in)) return -EIO;
        in.i_mode = (in.i_mode & S_IFMT) | (a2 & 07777);
        in.i_ctime = now();
        return write_inode(a1, &in);
    case FS_OP_STATFS:
        return op_statfs();
    case FS_OP_CHOWN:
        if (read_inode(a1, &in)) return -EIO;
        if ((int32_t)a2 != -1) { in.i_uid = a2 & 0xffff; in.i_uid_high = a2 >> 16; }
        if ((int32_t)a3 != -1) { in.i_gid = a3 & 0xffff; in.i_gid_high = a3 >> 16; }
        in.i_ctime = now();
        return write_inode(a1, &in);
    case FS_OP_SETTIMES:
        if (read_inode(a1, &in)) return -EIO;
        if ((int64_t)a2 >= 0) in.i_atime = a2;
        if ((int64_t)a3 >= 0) in.i_mtime = a3;
        in.i_ctime = now();
        return write_inode(a1, &in);
    case FS_OP_SETCRED:
        cred_uid = a1;
        cred_gid = a2;
        cred_ngroups = a3 < 32 ? a3 : 32;
        for (uint32_t i = 0; i < cred_ngroups; i++) cred_groups[i] = microkit_mr_get(4 + i);
        return 0;
    case FS_OP_SYNC:
        return op_sync(a1 == 1);
    case FS_OP_SETCLOCK:
        clk_epoch = a1; clk_tsc0 = a2; clk_hz = a3;
        return 0;
    case FS_OP_STATS:
        return op_stats(buf, FS_BUF_SIZE);
    case FS_OP_RAW:
        return op_raw(a1, a2, a3, (uint8_t *)buf);
    case FS_OP_REMOUNT:
        return op_remount(a1 == 1);
    case FS_OP_STATE:
        microkit_mr_set(1, (read_only ? FS_STATE_RO : 0) | (fs_dirty ? FS_STATE_DIRTY : 0));
        return 0;
    }
    return -EINVAL;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    long op = microkit_mr_get(0);
    long r;
    if (!mounted && op != FS_OP_SETCLOCK) r = -EIO;
    else if (read_only && is_write_op(op)) r = -EROFS;
    else {
        uint64_t t0 = tsc();
        op_freed = false;
        r = dispatch(op, microkit_mr_get(1), microkit_mr_get(2), microkit_mr_get(3));
        uint64_t t1 = tsc();
        if (op_freed || ndirty >= DIRTY_MAX || op == FS_OP_REMOUNT) {
            if (flush_meta()) {
                if (r >= 0) r = -EIO;
                flush_err = -EIO;
            }
        }
        uint64_t t2 = tsc();
        if (op != FS_OP_STATS) {
            st_op[op & 31].n++;
            st_op[op & 31].cyc += t2 - t0;
            st_flush.n++;
            st_flush.cyc += t2 - t1;
        }
    }
    microkit_mr_set(0, r);
    return microkit_msginfo_new(0, 5);
}

/* ------------------------------------------------------------------ */
/* Mounting                                                            */
/* ------------------------------------------------------------------ */

static void log_(const char *s) { microkit_dbg_puts(s); }

static void log_num(uint64_t v)
{
    char tmp[24];
    int i = 0;
    do { tmp[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i) microkit_dbg_putc(tmp[--i]);
}

/* After an unclean shutdown (power cut, QEMU closed) the free counts in the group descriptors
 * and the superblock can disagree with the bitmaps, which are the truth (every write goes
 * straight to disk): recount them, as e2fsck's pass 5 does */
static int recount_free(void)
{
    uint32_t fb = 0, fi = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        uint64_t first = sb->s_first_data_block + (uint64_t)g * sb->s_blocks_per_group;
        uint32_t nblocks = sb->s_blocks_count - first < sb->s_blocks_per_group
                         ? sb->s_blocks_count - first : sb->s_blocks_per_group;
        int64_t b = bitmap_count_free(gd[g].bg_block_bitmap, nblocks);
        int64_t i = bitmap_count_free(gd[g].bg_inode_bitmap, sb->s_inodes_per_group);
        if (b < 0 || i < 0) return -EIO;
        gd[g].bg_free_blocks_count = b;
        gd[g].bg_free_inodes_count = i;
        fb += b;
        fi += i;
    }
    sb->s_free_blocks_count = fb;
    sb->s_free_inodes_count = fi;
    gd_dirty = sb_dirty = true;
    return flush_meta();
}

/* FS_OP_SYNC: every operation is already on disk when it returns; unmounting marks it clean */
static long op_sync(bool unmount)
{
    if (flush_err) { flush_err = 0; return -EIO; }
    if (read_only) return flush_dirty() ? -EIO : 0;     /* e2fsck's raw writes */
    if (unmount) {
        sb->s_state |= EXT2_VALID_FS;
        sb_dirty = true;
    }
    if (flush_meta_(true)) return -EIO;
    if (unmount) read_only = true;
    return 0;
}

/* The superblock and the group descriptors, read and checked (at mount, and again when going
 * read-write after e2fsck). 0, or -EIO with the reason logged */
static int load_meta(void)
{
    int s = bread(0);
    if (s < 0) { log_("fs_ext2: could not read the disk\n"); return -EIO; }
    memcpy(sb_raw, slot_data(s) + 1024, sizeof(sb_raw));
    if (sb->s_magic != EXT2_MAGIC) { log_("fs_ext2: the partition is not ext2\n"); return -EIO; }
    if ((1024u << sb->s_log_block_size) != BS) { log_("fs_ext2: only 4 KiB blocks are supported\n"); return -EIO; }
    if (sb->s_feature_incompat & ~INCOMPAT_FILETYPE) {
        log_("fs_ext2: incompatible features (ext3/ext4?), not mounting\n");
        return -EIO;
    }
    ro_features = sb->s_feature_ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGE_FILE | RO_COMPAT_BTREE_DIR);
    inode_size = sb->s_rev_level >= 1 ? sb->s_inode_size : 128;
    ngroups = (sb->s_blocks_count - sb->s_first_data_block + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;
    if (ngroups > MAX_GROUPS) { log_("fs_ext2: too many block groups\n"); return -EIO; }

    uint32_t nbytes = ngroups * sizeof(struct ext2_gd);
    for (uint32_t off = 0; off < nbytes; off += BS) {
        s = bread(sb->s_first_data_block + 1 + off / BS);
        if (s < 0) { log_("fs_ext2: error reading group descriptors\n"); return -EIO; }
        memcpy((uint8_t *)gd + off, slot_data(s), nbytes - off < BS ? nbytes - off : BS);
    }
    return 0;
}

static void mount(void)
{
    if (load_meta()) return;
    if (ro_features) {
        log_("fs_ext2: unknown features: mounting read-only\n");
        read_only = true;
    } else if (!(sb->s_state & EXT2_VALID_FS)) {
        /* /etc/rc runs e2fsck on /dev/root and then goes read-write (FS_OP_REMOUNT) */
        log_("fs_ext2: the disk was not cleanly unmounted: read-only until e2fsck has checked it\n");
        read_only = fs_dirty = true;
    } else {
        sb->s_state &= ~EXT2_VALID_FS;            /* in use: clean again only after FS_OP_SYNC(1) */
        sb_dirty = true;
        if (flush_meta()) { log_("fs_ext2: cannot write the superblock: read-only\n"); read_only = true; }
    }
    mounted = true;
    log_("fs_ext2: mounted \"");
    char label[17];
    memcpy(label, sb->s_volume_name, 16);
    label[16] = 0;
    log_(label);
    log_("\": ");
    log_num((uint64_t)sb->s_blocks_count * BS / (1024 * 1024));
    log_(" MiB, ");
    log_num((uint64_t)sb->s_free_blocks_count * BS / (1024 * 1024));
    log_(" MiB free, ");
    log_num(sb->s_free_inodes_count);
    log_(" free inodes");
    log_(read_only ? " (read-only)\n" : "\n");
}

void init(void)
{
#ifdef FS_RAMDISK
    nslots = RAM_CACHE_SIZE / BS - STAGE_BLOCKS;
    if (nslots > MAX_SLOTS) nslots = MAX_SLOTS;
    stage_first = RAM_CACHE_SIZE / BS - STAGE_BLOCKS;
    cache_init();
    disk_blocks = (ramdisk_end - ramdisk_start) / BS;
    log_("fs_ext2: live system: the file system is in RAM (no disk is touched)\n");
    mount();
    return;
#endif
    if (!blk_config_check_magic(&blk_config)) {
        log_("fs_ext2: invalid block configuration\n");
        return;
    }
    blk_queue_init(&blkq, blk_config.virt.req_queue.vaddr, blk_config.virt.resp_queue.vaddr,
                   blk_config.virt.num_buffers);
    nslots = blk_config.data.size / BS - STAGE_BLOCKS;
    if (nslots > MAX_SLOTS) nslots = MAX_SLOTS;
    stage_first = blk_config.data.size / BS - STAGE_BLOCKS;
    cache_init();

    blk_storage_info_t *info = blk_config.virt.storage_info.vaddr;
    while (!blk_storage_is_ready(info)) __asm__ volatile("pause" ::: "memory");
    disk_blocks = info->capacity;
    mount();
}

void notified(microkit_channel ch)
{
#ifdef FS_RAMDISK
    (void)blkq;
#endif
    (void)ch;   /* blk_virt's replies are handled by polling in blk_io() */
}
