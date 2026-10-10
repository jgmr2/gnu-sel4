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
 *   - block cache with immediate writes (write-through), no journal
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

static uint32_t now(void)
{
    if (!clk_hz) return clk_epoch;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return clk_epoch + (((uint64_t)hi << 32 | lo) - clk_tsc0) / clk_hz;
}

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
static uint8_t sb_raw[1024];           /* copy of the superblock (bytes 1024..2047 of the disk) */
static struct ext2_super *sb = (struct ext2_super *)sb_raw;
static uint32_t ngroups, inode_size;
#define MAX_GROUPS 1024
static struct ext2_gd gd[MAX_GROUPS];
static bool sb_dirty, gd_dirty;

/* ------------------------------------------------------------------ */
/* Block cache over the sDDF data region                               */
/* ------------------------------------------------------------------ */

#define MAX_SLOTS 1024
static struct { uint64_t blk; uint32_t lru; bool valid; } slots[MAX_SLOTS];
static uint32_t nslots, lru_clock;

static uint8_t *slot_data(int s) { return (uint8_t *)blk_config.data.vaddr + (uintptr_t)s * BS; }

/* One request at a time: queue it, notify blk_virt and wait for the reply.
 * blk_virt and the driver have a higher priority, so they run while this
 * component waits. */
static int blk_io(blk_req_code_t code, int s, uint64_t blk)
{
    if (blk >= disk_blocks) return -EIO;
    if (blk_enqueue_req(&blkq, code, (uintptr_t)s * BS, blk, 1, 0)) return -EIO;
    sddf_notify(blk_config.virt.id);
    while (blk_queue_empty_resp(&blkq)) __asm__ volatile("pause" ::: "memory");
    blk_resp_status_t status;
    uint16_t count;
    uint32_t id;
    blk_dequeue_resp(&blkq, &status, &count, &id);
    return status == BLK_RESP_OK ? 0 : -EIO;
}

static int cache_slot(uint64_t blk, bool *hit)
{
    int victim = 0;
    for (uint32_t i = 0; i < nslots; i++) {
        if (slots[i].valid && slots[i].blk == blk) {
            slots[i].lru = ++lru_clock;
            *hit = true;
            return i;
        }
        if (!slots[i].valid) victim = i;
        else if (slots[victim].valid && slots[i].lru < slots[victim].lru) victim = i;
    }
    *hit = false;
    slots[victim].valid = false;
    slots[victim].blk = blk;
    slots[victim].lru = ++lru_clock;
    return victim;
}

/* Block in the cache (read from disk if needed). Returns the slot or -EIO. */
static int bread(uint64_t blk)
{
    bool hit;
    int s = cache_slot(blk, &hit);
    if (!hit) {
        if (blk_io(BLK_REQ_READ, s, blk)) return -EIO;
        slots[s].valid = true;
    }
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

static int bwrite(int s)
{
    return blk_io(BLK_REQ_WRITE, s, slots[s].blk);
}

/* ------------------------------------------------------------------ */
/* Metadata: superblock, group descriptors, inodes                     */
/* ------------------------------------------------------------------ */

static int flush_meta(void)
{
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
    return 0;
}

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

static int64_t bitmap_alloc(uint64_t bitmap_blk, uint32_t nbits)
{
    int s = bread(bitmap_blk);
    if (s < 0) return s;
    uint8_t *bm = slot_data(s);
    for (uint32_t i = 0; i < nbits; i++) {
        if (!(bm[i / 8] & (1u << (i % 8)))) {
            bm[i / 8] |= 1u << (i % 8);
            if (bwrite(s)) return -EIO;
            return i;
        }
    }
    return -ENOSPC;
}

static int bitmap_free(uint64_t bitmap_blk, uint32_t bit)
{
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
        int64_t bit = bitmap_alloc(gd[g].bg_block_bitmap, sb->s_blocks_per_group);
        if (bit == -ENOSPC) continue;
        if (bit < 0) return bit;
        gd[g].bg_free_blocks_count--;
        sb->s_free_blocks_count--;
        gd_dirty = sb_dirty = true;
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
    gd_dirty = sb_dirty = true;
    return 0;
}

static uint32_t inode_group(uint32_t ino) { return (ino - 1) / sb->s_inodes_per_group; }

static int64_t alloc_inode(uint32_t goal_group, bool is_dir)
{
    if (!sb->s_free_inodes_count) return -ENOSPC;
    for (uint32_t i = 0; i < ngroups; i++) {
        uint32_t g = (goal_group + i) % ngroups;
        if (!gd[g].bg_free_inodes_count) continue;
        int64_t bit = bitmap_alloc(gd[g].bg_inode_bitmap, sb->s_inodes_per_group);
        if (bit == -ENOSPC) continue;
        if (bit < 0) return bit;
        gd[g].bg_free_inodes_count--;
        if (is_dir) gd[g].bg_used_dirs_count++;
        sb->s_free_inodes_count--;
        gd_dirty = sb_dirty = true;
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
    gd_dirty = sb_dirty = true;
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

static long file_read(uint32_t ino, struct ext2_inode *in, uint64_t off, uint32_t len, uint8_t *out)
{
    uint64_t size = inode_size_of(in);
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
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
        int s = bread(pb);
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

static long op_create(const char *path, uint32_t mode)
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

static long op_symlink(const char *target, const char *path)
{
    size_t tlen = strlen(target);
    if (tlen >= BS) return -ENAMETOOLONG;
    int64_t ino = op_create(path, S_IFLNK | 0777);
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
    return 0;
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
        return op_create(buf, a1);
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
        return op_symlink(buf, buf + FS_BUF_PATH2);
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
    case FS_OP_SETCLOCK:
        clk_epoch = a1; clk_tsc0 = a2; clk_hz = a3;
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
        r = dispatch(op, microkit_mr_get(1), microkit_mr_get(2), microkit_mr_get(3));
        if (flush_meta() && r >= 0) r = -EIO;
    }
    microkit_mr_set(0, r);
    return microkit_msginfo_new(0, 4);
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

static void mount(void)
{
    int s = bread(0);
    if (s < 0) { log_("fs_ext2: could not read the disk\n"); return; }
    memcpy(sb_raw, slot_data(s) + 1024, sizeof(sb_raw));
    if (sb->s_magic != EXT2_MAGIC) { log_("fs_ext2: the partition is not ext2\n"); return; }
    if ((1024u << sb->s_log_block_size) != BS) { log_("fs_ext2: only 4 KiB blocks are supported\n"); return; }
    if (sb->s_feature_incompat & ~INCOMPAT_FILETYPE) {
        log_("fs_ext2: incompatible features (ext3/ext4?), not mounting\n");
        return;
    }
    if (sb->s_feature_ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGE_FILE | RO_COMPAT_BTREE_DIR)) {
        log_("fs_ext2: unknown features: mounting read-only\n");
        read_only = true;
    }
    inode_size = sb->s_rev_level >= 1 ? sb->s_inode_size : 128;
    ngroups = (sb->s_blocks_count - sb->s_first_data_block + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;
    if (ngroups > MAX_GROUPS) { log_("fs_ext2: too many block groups\n"); return; }

    uint32_t nbytes = ngroups * sizeof(struct ext2_gd);
    for (uint32_t off = 0; off < nbytes; off += BS) {
        s = bread(sb->s_first_data_block + 1 + off / BS);
        if (s < 0) { log_("fs_ext2: error reading group descriptors\n"); return; }
        memcpy((uint8_t *)gd + off, slot_data(s), nbytes - off < BS ? nbytes - off : BS);
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
    if (!blk_config_check_magic(&blk_config)) {
        log_("fs_ext2: invalid block configuration\n");
        return;
    }
    blk_queue_init(&blkq, blk_config.virt.req_queue.vaddr, blk_config.virt.resp_queue.vaddr,
                   blk_config.virt.num_buffers);
    nslots = blk_config.data.size / BS;
    if (nslots > MAX_SLOTS) nslots = MAX_SLOTS;

    blk_storage_info_t *info = blk_config.virt.storage_info.vaddr;
    while (!blk_storage_is_ready(info)) __asm__ volatile("pause" ::: "memory");
    disk_blocks = info->capacity;
    mount();
}

void notified(microkit_channel ch)
{
    (void)ch;   /* blk_virt's replies are handled by polling in blk_io() */
}
