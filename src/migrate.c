/*
 * v1 -> v2 migration, run automatically at mount time.
 *
 * Crash-restartable: each inode is migrated in its own transaction and
 * marked with SFS_IFLAG_EXTENTS; an interrupted migration simply resumes,
 * skipping flagged inodes. The superblock version flips to 2 in a final
 * transaction together with the engineering-log entry, so the log is
 * written exactly once.
 *
 * Files adopt their existing data blocks in place (consecutive v1 block
 * pointers are coalesced into extents; no data moves). Only if a file is
 * so fragmented that it exceeds the extent limit is its data rewritten
 * into fresh extents. Directories are rebuilt from the v1 linear entry
 * list into the hash-chained v2 index.
 */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bit_get(const uint8_t *map, uint64_t idx)
{
    return (map[idx >> 3] >> (idx & 7)) & 1;
}

/* ordered physical block list of a v1 inode (no holes in v1 files) */
static int v1_block_list(sfs_fs_t *fs, const sfs_inode_t *in,
                         uint32_t **out, uint32_t *n_out)
{
    uint32_t n = (uint32_t)((in->size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE);
    uint32_t *blks = malloc(n ? (size_t)n * sizeof(uint32_t) : 1);
    if (!blks) return -ENOMEM;
    uint8_t ind[SFS_BLOCK_SIZE];
    int have_ind = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b;
        if (i < SFS_NDIRECT_V1) {
            b = in->v1_direct[i];
        } else {
            if (!have_ind) {
                if (in->v1_indirect == 0) { free(blks); return -EIO; }
                int rc = sfs_bread(fs, in->v1_indirect, ind);
                if (rc) { free(blks); return rc; }
                have_ind = 1;
            }
            b = get_le32(ind + (i - SFS_NDIRECT_V1) * 4);
        }
        if (b == 0) { free(blks); return -EIO; }
        blks[i] = b;
    }
    *out = blks;
    *n_out = n;
    return 0;
}

static uint32_t count_runs(const uint32_t *blks, uint32_t n)
{
    uint32_t runs = 0;
    for (uint32_t i = 0; i < n; i++)
        if (i == 0 || blks[i] != blks[i - 1] + 1)
            runs++;
    return runs;
}

static void reset_map(sfs_inode_t *in)
{
    in->nextents = 0;
    in->ext_block = 0;
    memset(in->ext, 0, sizeof(in->ext));
}

/* free the old v1 blocks, run by run */
static int free_v1_blocks(sfs_fs_t *fs, const uint32_t *blks, uint32_t n,
                          uint32_t indirect)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t cnt = 1;
        while (i + cnt < n && blks[i + cnt] == blks[i] + cnt)
            cnt++;
        int rc = sfs_ext_free(fs, blks[i], cnt);
        if (rc) return rc;
        i += cnt;
    }
    if (indirect)
        return sfs_ext_free(fs, indirect, 1);
    return 0;
}

static int migrate_file(sfs_fs_t *fs, uint32_t ino, sfs_inode_t *in)
{
    uint32_t *blks, n;
    int rc = v1_block_list(fs, in, &blks, &n);
    if (rc) return rc;

    uint64_t size = in->size;
    reset_map(in);

    if (count_runs(blks, n) <= SFS_MAX_EXTENTS) {
        /* adopt blocks in place: no data movement */
        uint32_t i = 0;
        while (i < n && rc == 0) {
            sfs_extent_t e;
            e.start = blks[i];
            e.count = 1;
            while (i + e.count < n && blks[i + e.count] == e.start + e.count)
                e.count++;
            rc = sfs_ext_append(fs, in, e);
            i += e.count;
        }
        if (rc == 0 && in->v1_indirect)
            rc = sfs_ext_free(fs, in->v1_indirect, 1);
    } else {
        /* pathological fragmentation: rewrite into fresh extents
         * (freed blocks re-enter the allocator only at commit, so the
         * old data stays intact if this transaction is rolled back) */
        uint8_t *data = malloc((size_t)n * SFS_BLOCK_SIZE);
        if (!data) { free(blks); return -ENOMEM; }
        for (uint32_t i = 0; i < n && rc == 0; i++)
            rc = sfs_bread(fs, blks[i], data + (size_t)i * SFS_BLOCK_SIZE);
        if (rc == 0)
            rc = free_v1_blocks(fs, blks, n, in->v1_indirect);
        uint32_t have = 0;
        while (rc == 0 && have < n) {
            sfs_extent_t e;
            rc = sfs_ext_alloc(fs, n - have, &e);
            if (rc) break;
            rc = sfs_ext_append(fs, in, e);
            for (uint32_t j = 0; j < e.count && rc == 0; j++)
                rc = sfs_bput(fs, e.start + j,
                              data + (size_t)(have + j) * SFS_BLOCK_SIZE);
            have += e.count;
        }
        free(data);
    }
    free(blks);
    if (rc) return rc;

    in->flags |= SFS_IFLAG_EXTENTS;
    in->size = size;
    in->v1_indirect = 0;
    return sfs_iput(fs, ino, in);
}

static int migrate_dir(sfs_fs_t *fs, uint32_t ino, sfs_inode_t *in)
{
    uint32_t *blks, n;
    int rc = v1_block_list(fs, in, &blks, &n);
    if (rc) return rc;

    /* collect live entries from the v1 linear list (v1 dirents share the
     * v2 on-disk layout except the chain field, which v1 left zero) */
    sfs_dirent_t *ents = malloc((size_t)n * SFS_DIRENTS_PER_BLK *
                                sizeof(sfs_dirent_t) + 1);
    if (!ents) { free(blks); return -ENOMEM; }
    uint32_t nent = 0;
    uint8_t buf[SFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < n && rc == 0; i++) {
        rc = sfs_bread(fs, blks[i], buf);
        for (unsigned s = 0; rc == 0 && s < SFS_DIRENTS_PER_BLK; s++) {
            sfs_dirent_t de;
            sfs_dirent_decode(&de, buf + s * SFS_DIRENT_SIZE);
            if (de.ino != 0)
                ents[nent++] = de;
        }
    }
    if (rc == 0)
        rc = free_v1_blocks(fs, blks, n, in->v1_indirect);
    free(blks);

    if (rc == 0) {
        uint64_t mtime = in->mtime, atime = in->atime;
        reset_map(in);
        in->size = 0;
        rc = sfs_dir_init(fs, in);
        for (uint32_t i = 0; i < nent && rc == 0; i++)
            rc = sfs_dir_add(fs, ino, in, ents[i].name, ents[i].ino);
        in->mtime = mtime;        /* preserve original timestamps */
        in->atime = atime;
    }
    free(ents);
    if (rc) return rc;

    in->flags |= SFS_IFLAG_EXTENTS;
    in->v1_indirect = 0;
    return sfs_iput(fs, ino, in);
}

int sfs_migrate_v1(sfs_fs_t *fs)
{
    if (fs->sb.version != SFS_VERSION_V1)
        return 0;

    uint32_t nfiles = 0, ndirs = 0;
    uint8_t iblk[SFS_BLOCK_SIZE];
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (!bit_get(fs->imap, ino))
            continue;
        int rc = sfs_bread(fs, fs->sb.inode_start + ino / SFS_INODES_PER_BLK,
                           iblk);
        if (rc) return rc;
        const uint8_t *raw = iblk +
            (ino % SFS_INODES_PER_BLK) * SFS_INODE_SIZE;
        sfs_inode_t in;
        sfs_inode_decode(&in, raw);
        if (in.type == SFS_TYPE_FREE || (in.flags & SFS_IFLAG_EXTENTS))
            continue;   /* already migrated (crash-restart) */
        sfs_inode_decode_v1(&in, raw);

        rc = sfs_txn_begin(fs);
        if (rc) return rc;
        rc = in.type == SFS_TYPE_DIR ? migrate_dir(fs, ino, &in)
                                     : migrate_file(fs, ino, &in);
        if (rc == 0)
            rc = sfs_txn_commit(fs);
        else
            sfs_txn_abort(fs);
        if (rc) return rc;
        if (in.type == SFS_TYPE_DIR) ndirs++; else nfiles++;
    }

    /* finalize: engineering-log entry + version flip, atomically */
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;
    uint32_t dummy;
    if (sfs_resolve(fs, "/docs", &dummy) != 0) {
        rc = sfs_mkdir(fs, "/docs");   /* nested: joins this txn */
        if (rc) { sfs_txn_abort(fs); return rc; }
    }
    char log[640];
    snprintf(log, sizeof(log),
        "\n## [STEP M] v1 -> v2 migration (performed at mount)\n"
        "- Goal: upgrade this image to format_version 2 without data loss\n"
        "- migrated %u files and %u directories to extent mapping\n"
        "- file data blocks adopted in place (runs coalesced into extents,"
        " no data movement); freed v1 indirect blocks\n"
        "- directories rebuilt into the hash-chained index"
        " (1023 buckets, chained 512-byte slots)\n"
        "- superblock format_version: 1 -> 2 in the same transaction as"
        " this log entry, so migration is exactly-once and"
        " crash-restartable via the per-inode extent flag\n",
        nfiles, ndirs);
    rc = sfs_write_file(fs, "/docs/slopfs_engineering_log.md",
                        log, strlen(log), 1);
    if (rc) {
        if (sfs_in_txn(fs))
            sfs_txn_abort(fs);
        return rc;
    }
    fs->sb.version = SFS_VERSION_V2;
    return sfs_txn_commit(fs);
}
