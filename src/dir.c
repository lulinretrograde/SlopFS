/*
 * Directories (v2): hash-chained index.
 *
 * Logical block 0 of every directory holds 1023 u32 bucket heads plus a
 * free-slot search hint in the last 4 bytes. Entry slots (512-byte
 * dirents, 8 per block) start at logical block 1; slot s lives at
 * logical block 1+s/8. Each dirent carries the next slot in its hash
 * chain (SFS_DIR_EOC terminates). Lookup/insert/remove are amortized
 * O(1); iteration in slot order gives a stable ls order.
 */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <string.h>

static uint32_t dir_nslots(const sfs_inode_t *dir)
{
    uint64_t blocks = dir->size / SFS_BLOCK_SIZE;
    return blocks > 0 ? (uint32_t)(blocks - 1) * SFS_DIRENTS_PER_BLK : 0;
}

static int dir_lblk_phys(sfs_fs_t *fs, const sfs_inode_t *dir,
                         uint32_t lblk, uint64_t *phys)
{
    int rc = sfs_inode_lookup_blk(fs, dir, lblk, phys);
    if (rc) return rc;
    return *phys == 0 ? -EIO : 0;   /* directories have no holes */
}

static int slot_read(sfs_fs_t *fs, const sfs_inode_t *dir, uint32_t slot,
                     sfs_dirent_t *de)
{
    uint64_t phys;
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 1 + slot / SFS_DIRENTS_PER_BLK, &phys);
    if (rc) return rc;
    rc = sfs_bread(fs, phys, buf);
    if (rc) return rc;
    sfs_dirent_decode(de, buf + (slot % SFS_DIRENTS_PER_BLK) * SFS_DIRENT_SIZE);
    return 0;
}

static int slot_write(sfs_fs_t *fs, const sfs_inode_t *dir, uint32_t slot,
                      const sfs_dirent_t *de)
{
    uint64_t phys;
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 1 + slot / SFS_DIRENTS_PER_BLK, &phys);
    if (rc) return rc;
    rc = sfs_bread(fs, phys, buf);
    if (rc) return rc;
    sfs_dirent_encode(de, buf + (slot % SFS_DIRENTS_PER_BLK) * SFS_DIRENT_SIZE);
    return sfs_bput(fs, phys, buf);
}

int sfs_dir_init(sfs_fs_t *fs, sfs_inode_t *dir)
{
    sfs_extent_t e;
    int rc = sfs_ext_alloc(fs, 1, &e);
    if (rc) return rc;
    uint8_t buf[SFS_BLOCK_SIZE];
    for (unsigned b = 0; b < SFS_DIR_NBUCKETS; b++)
        put_le32(buf + b * 4, SFS_DIR_EOC);
    put_le32(buf + SFS_DIR_HINT_OFF, 0);   /* free-slot hint */
    rc = sfs_bput(fs, e.start, buf);
    if (rc) return rc;
    rc = sfs_ext_append(fs, dir, e);
    if (rc) return rc;
    dir->size = SFS_BLOCK_SIZE;
    return 0;
}

int sfs_dir_lookup(sfs_fs_t *fs, const sfs_inode_t *dir, const char *name,
                   uint32_t *ino_out)
{
    if (dir->type != SFS_TYPE_DIR)
        return -ENOTDIR;
    if (dir->size < SFS_BLOCK_SIZE)
        return -ENOENT;
    uint64_t bphys;
    uint8_t bb[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 0, &bphys);
    if (rc) return rc;
    rc = sfs_bread(fs, bphys, bb);
    if (rc) return rc;

    uint32_t nslots = dir_nslots(dir);
    uint32_t slot = get_le32(bb + (sfs_name_hash(name) % SFS_DIR_NBUCKETS) * 4);
    uint32_t hops = 0;
    while (slot != SFS_DIR_EOC) {
        if (slot >= nslots || ++hops > nslots)
            return -EIO;   /* corrupt chain */
        sfs_dirent_t de;
        rc = slot_read(fs, dir, slot, &de);
        if (rc) return rc;
        if (de.ino != 0 && strcmp(de.name, name) == 0) {
            *ino_out = de.ino;
            return 0;
        }
        slot = de.next;
    }
    return -ENOENT;
}

/* find a free slot starting at the hint, growing the directory if every
 * slot is taken */
static int find_free_slot(sfs_fs_t *fs, sfs_inode_t *dir, uint8_t *bb,
                          uint32_t *slot_out)
{
    uint32_t nslots = dir_nslots(dir);
    uint32_t hint = get_le32(bb + SFS_DIR_HINT_OFF);
    if (hint > nslots) hint = nslots;

    for (uint32_t s = hint; s < nslots; s++) {
        sfs_dirent_t de;
        int rc = slot_read(fs, dir, s, &de);
        if (rc) return rc;
        if (de.ino == 0) {
            *slot_out = s;
            return 0;
        }
    }

    /* extend: try to grow the last extent for contiguity first.
     * Growth is geometric (capped at 64 blocks): one block at a time
     * gives a busy directory one extent per block under interleaved
     * allocation and hits the 522-extent cap at ~4k entries; chunked
     * growth keeps it at ~nblocks/64 extents. */
    uint32_t cur = (uint32_t)(dir->size / SFS_BLOCK_SIZE);
    uint32_t chunk = cur > 64 ? 64 : (cur ? cur : 1);
    sfs_extent_t last, e;
    int rc = sfs_ext_get(fs, dir, dir->nextents - 1, &last);
    if (rc) return rc;
    uint32_t got = sfs_ext_alloc_at(fs, last.start + last.count, chunk);
    if (got > 0) {
        e.start = last.start + last.count;
        e.count = got;
    } else {
        rc = sfs_ext_alloc(fs, chunk, &e);
        if (rc) return rc;
    }
    uint8_t zero[SFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < e.count; i++) {
        rc = sfs_bput(fs, e.start + i, zero);
        if (rc) return rc;
    }
    rc = sfs_ext_append(fs, dir, e);
    if (rc) return rc;
    dir->size += (uint64_t)e.count * SFS_BLOCK_SIZE;
    *slot_out = nslots;
    return 0;
}

int sfs_dir_add(sfs_fs_t *fs, uint32_t dir_ino, sfs_inode_t *dir,
                const char *name, uint32_t ino)
{
    (void)dir_ino;
    size_t namelen = strnlen(name, SFS_NAME_MAX + 1);
    if (namelen == 0 || namelen > SFS_NAME_MAX)
        return -ENAMETOOLONG;
    if (dir->size < SFS_BLOCK_SIZE)
        return -EIO;

    uint64_t bphys;
    uint8_t bb[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 0, &bphys);
    if (rc) return rc;
    rc = sfs_bread(fs, bphys, bb);
    if (rc) return rc;

    uint32_t slot;
    rc = find_free_slot(fs, dir, bb, &slot);
    if (rc) return rc;

    uint32_t b = sfs_name_hash(name) % SFS_DIR_NBUCKETS;
    sfs_dirent_t de;
    de.ino = ino;
    de.next = get_le32(bb + b * 4);   /* prepend to chain */
    memcpy(de.name, name, namelen);
    de.name[namelen] = '\0';
    rc = slot_write(fs, dir, slot, &de);
    if (rc) return rc;

    put_le32(bb + b * 4, slot);
    put_le32(bb + SFS_DIR_HINT_OFF, slot + 1);
    rc = sfs_bput(fs, bphys, bb);
    if (rc) return rc;

    dir->mtime = sfs_now(fs);
    return 0;
}

int sfs_dir_remove(sfs_fs_t *fs, uint32_t dir_ino, sfs_inode_t *dir,
                   const char *name)
{
    (void)dir_ino;
    if (dir->size < SFS_BLOCK_SIZE)
        return -ENOENT;
    uint64_t bphys;
    uint8_t bb[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 0, &bphys);
    if (rc) return rc;
    rc = sfs_bread(fs, bphys, bb);
    if (rc) return rc;

    uint32_t nslots = dir_nslots(dir);
    uint32_t b = sfs_name_hash(name) % SFS_DIR_NBUCKETS;
    uint32_t slot = get_le32(bb + b * 4);
    uint32_t prev = SFS_DIR_EOC;
    sfs_dirent_t de, prevde;
    uint32_t hops = 0;

    while (slot != SFS_DIR_EOC) {
        if (slot >= nslots || ++hops > nslots)
            return -EIO;
        rc = slot_read(fs, dir, slot, &de);
        if (rc) return rc;
        if (de.ino != 0 && strcmp(de.name, name) == 0)
            break;
        prev = slot;
        prevde = de;
        slot = de.next;
    }
    if (slot == SFS_DIR_EOC)
        return -ENOENT;

    if (prev == SFS_DIR_EOC) {
        put_le32(bb + b * 4, de.next);   /* head of chain */
    } else {
        prevde.next = de.next;
        rc = slot_write(fs, dir, prev, &prevde);
        if (rc) return rc;
    }
    memset(&de, 0, sizeof(de));
    de.next = SFS_DIR_EOC;
    de.ino = 0;
    rc = slot_write(fs, dir, slot, &de);
    if (rc) return rc;

    uint32_t hint = get_le32(bb + SFS_DIR_HINT_OFF);
    if (slot < hint)
        put_le32(bb + SFS_DIR_HINT_OFF, slot);
    rc = sfs_bput(fs, bphys, bb);
    if (rc) return rc;

    dir->mtime = sfs_now(fs);
    return 0;
}

int sfs_dir_iterate(sfs_fs_t *fs, const sfs_inode_t *dir,
                    sfs_dir_cb cb, void *arg)
{
    if (dir->type != SFS_TYPE_DIR)
        return -ENOTDIR;
    uint32_t nslots = dir_nslots(dir);
    uint8_t buf[SFS_BLOCK_SIZE];
    for (uint32_t s = 0; s < nslots; s++) {
        if (s % SFS_DIRENTS_PER_BLK == 0) {
            uint64_t phys;
            int rc = dir_lblk_phys(fs, dir, 1 + s / SFS_DIRENTS_PER_BLK,
                                   &phys);
            if (rc) return rc;
            rc = sfs_bread(fs, phys, buf);
            if (rc) return rc;
        }
        sfs_dirent_t de;
        sfs_dirent_decode(&de, buf + (s % SFS_DIRENTS_PER_BLK) *
                                   SFS_DIRENT_SIZE);
        if (de.ino == 0)
            continue;
        if (cb(&de, arg))
            return 0;
    }
    return 0;
}

int sfs_dir_is_empty(sfs_fs_t *fs, const sfs_inode_t *dir)
{
    if (dir->size < SFS_BLOCK_SIZE)
        return 1;
    uint64_t bphys;
    uint8_t bb[SFS_BLOCK_SIZE];
    int rc = dir_lblk_phys(fs, dir, 0, &bphys);
    if (rc) return rc;
    rc = sfs_bread(fs, bphys, bb);
    if (rc) return rc;
    for (unsigned b = 0; b < SFS_DIR_NBUCKETS; b++)
        if (get_le32(bb + b * 4) != SFS_DIR_EOC)
            return 0;
    return 1;
}
