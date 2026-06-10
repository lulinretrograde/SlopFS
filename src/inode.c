/* Inode table access and extent mapping (v2). */
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <string.h>

static uint64_t ino_block(const sfs_fs_t *fs, uint32_t ino)
{
    return fs->sb.inode_start + ino / SFS_INODES_PER_BLK;
}
static unsigned ino_offset(uint32_t ino)
{
    return (ino % SFS_INODES_PER_BLK) * SFS_INODE_SIZE;
}

int sfs_iget(sfs_fs_t *fs, uint32_t ino, sfs_inode_t *out)
{
    if (ino == 0 || ino >= fs->sb.inode_count)
        return -EINVAL;
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = sfs_bread(fs, ino_block(fs, ino), buf);
    if (rc) return rc;
    sfs_inode_decode(out, buf + ino_offset(ino));
    return 0;
}

int sfs_iput(sfs_fs_t *fs, uint32_t ino, const sfs_inode_t *in)
{
    if (ino == 0 || ino >= fs->sb.inode_count)
        return -EINVAL;
    uint8_t buf[SFS_BLOCK_SIZE];
    uint64_t blk = ino_block(fs, ino);
    int rc = sfs_bread(fs, blk, buf);
    if (rc) return rc;
    sfs_inode_encode(in, buf + ino_offset(ino));
    return sfs_bstage(fs, blk, buf);
}

int sfs_ext_get(sfs_fs_t *fs, const sfs_inode_t *in, uint32_t idx,
                sfs_extent_t *out)
{
    if (idx >= in->nextents)
        return -EINVAL;
    if (idx < SFS_INLINE_EXTENTS) {
        *out = in->ext[idx];
        return 0;
    }
    if (in->ext_block == 0)
        return -EINVAL;
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = sfs_bread(fs, in->ext_block, buf);
    if (rc) return rc;
    uint32_t o = idx - SFS_INLINE_EXTENTS;
    out->start = get_le32(buf + o * 8);
    out->count = get_le32(buf + o * 8 + 4);
    return 0;
}

static int ext_set(sfs_fs_t *fs, sfs_inode_t *in, uint32_t idx,
                   sfs_extent_t e)
{
    if (idx < SFS_INLINE_EXTENTS) {
        in->ext[idx] = e;
        return 0;
    }
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc;
    if (in->ext_block == 0) {
        sfs_extent_t eb;
        rc = sfs_ext_alloc(fs, 1, &eb);
        if (rc) return rc;
        in->ext_block = eb.start;
        memset(buf, 0, sizeof(buf));
    } else {
        rc = sfs_bread(fs, in->ext_block, buf);
        if (rc) return rc;
    }
    uint32_t o = idx - SFS_INLINE_EXTENTS;
    put_le32(buf + o * 8, e.start);
    put_le32(buf + o * 8 + 4, e.count);
    return sfs_bput(fs, in->ext_block, buf);
}

int sfs_ext_append(sfs_fs_t *fs, sfs_inode_t *in, sfs_extent_t e)
{
    if (e.count == 0)
        return 0;
    if (in->nextents > 0) {
        sfs_extent_t last;
        int rc = sfs_ext_get(fs, in, in->nextents - 1, &last);
        if (rc) return rc;
        if (last.start + last.count == e.start) {   /* coalesce */
            last.count += e.count;
            return ext_set(fs, in, in->nextents - 1, last);
        }
    }
    if (in->nextents >= SFS_MAX_EXTENTS)
        return -EFBIG;
    int rc = ext_set(fs, in, in->nextents, e);
    if (rc) return rc;
    in->nextents++;
    return 0;
}

int sfs_inode_lookup_blk(sfs_fs_t *fs, const sfs_inode_t *in,
                         uint32_t lblk, uint64_t *phys)
{
    uint32_t off = 0;
    uint8_t buf[SFS_BLOCK_SIZE];
    int have_overflow = 0;
    for (uint32_t i = 0; i < in->nextents; i++) {
        sfs_extent_t e;
        if (i < SFS_INLINE_EXTENTS) {
            e = in->ext[i];
        } else {
            if (!have_overflow) {
                if (in->ext_block == 0)
                    return -EINVAL;
                int rc = sfs_bread(fs, in->ext_block, buf);
                if (rc) return rc;
                have_overflow = 1;
            }
            uint32_t o = i - SFS_INLINE_EXTENTS;
            e.start = get_le32(buf + o * 8);
            e.count = get_le32(buf + o * 8 + 4);
        }
        if (lblk < off + e.count) {
            *phys = (uint64_t)e.start + (lblk - off);
            return 0;
        }
        off += e.count;
    }
    *phys = 0;   /* beyond mapping */
    return 0;
}

int sfs_inode_nblocks(sfs_fs_t *fs, const sfs_inode_t *in, uint32_t *out)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < in->nextents; i++) {
        sfs_extent_t e;
        int rc = sfs_ext_get(fs, in, i, &e);
        if (rc) return rc;
        sum += e.count;
    }
    *out = sum;
    return 0;
}

int sfs_inode_shrink(sfs_fs_t *fs, sfs_inode_t *in, uint32_t keep)
{
    uint32_t off = 0;
    uint32_t new_n = 0;
    for (uint32_t i = 0; i < in->nextents; i++) {
        sfs_extent_t e;
        int rc = sfs_ext_get(fs, in, i, &e);
        if (rc) return rc;
        if (off + e.count <= keep) {
            off += e.count;
            new_n = i + 1;
            continue;
        }
        uint32_t head = keep > off ? keep - off : 0;
        rc = sfs_ext_free(fs, e.start + head, e.count - head);
        if (rc) return rc;
        off += e.count;
        if (head > 0) {           /* split: keep the extent's head */
            e.count = head;
            rc = ext_set(fs, in, i, e);
            if (rc) return rc;
            new_n = i + 1;
        }
    }
    in->nextents = new_n;
    if (new_n <= SFS_INLINE_EXTENTS && in->ext_block) {
        int rc = sfs_ext_free(fs, in->ext_block, 1);
        if (rc) return rc;
        in->ext_block = 0;
    }
    for (uint32_t i = new_n; i < SFS_INLINE_EXTENTS; i++)
        in->ext[i].start = in->ext[i].count = 0;
    return 0;
}

int sfs_inode_truncate(sfs_fs_t *fs, sfs_inode_t *in)
{
    int rc = sfs_inode_shrink(fs, in, 0);
    if (rc) return rc;
    in->size = 0;
    return 0;
}
