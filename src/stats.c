/* Filesystem statistics: free space, fragmentation, extent usage. */
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <string.h>

static int bit_get(const uint8_t *map, uint64_t idx)
{
    return (map[idx >> 3] >> (idx & 7)) & 1;
}

int sfs_collect_stats(sfs_fs_t *fs, sfs_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->total_blocks = fs->sb.total_blocks;
    st->free_blocks = fs->sb.free_blocks;
    st->data_blocks = fs->sb.total_blocks - fs->sb.data_start;
    st->largest_free_run = sfs_largest_free_run(fs);
    st->frag_ratio = st->free_blocks
        ? 1.0 - (double)st->largest_free_run / (double)st->free_blocks
        : 0.0;
    st->inode_count = fs->sb.inode_count;
    st->free_inodes = fs->sb.free_inodes;
    st->journal_blocks = fs->sb.journal_blocks;

    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = sfs_dev_read(&fs->dev, fs->sb.journal_start, buf);
    if (rc) return rc;
    if (memcmp(buf, SFS_JMAGIC, SFS_JMAGIC_LEN) == 0)
        st->journal_last_txn = get_le32(buf + 20);

    /* one pass over the inode table */
    uint64_t total_ext = 0;
    for (uint64_t b = 0; b < fs->sb.inode_blocks; b++) {
        rc = sfs_bread(fs, fs->sb.inode_start + b, buf);
        if (rc) return rc;
        for (unsigned i = 0; i < SFS_INODES_PER_BLK; i++) {
            uint32_t ino = (uint32_t)(b * SFS_INODES_PER_BLK + i);
            if (ino == 0 || ino >= fs->sb.inode_count)
                continue;
            if (!bit_get(fs->imap, ino))
                continue;
            sfs_inode_t in;
            sfs_inode_decode(&in, buf + i * SFS_INODE_SIZE);
            if (in.type == SFS_TYPE_FILE) {
                st->nfiles++;
                total_ext += in.nextents;
            } else if (in.type == SFS_TYPE_DIR) {
                st->ndirs++;
            }
        }
    }
    st->avg_extents_per_file =
        st->nfiles ? (double)total_ext / st->nfiles : 0.0;
    return 0;
}
