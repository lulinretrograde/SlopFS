/*
 * Write-ahead journal and transaction engine.
 *
 * A transaction stages full block images in memory. Commit protocol:
 *   1. write all images into the journal area          (intent log)
 *   2. fsync
 *   3. write journal header: state=COMMITTED + CRCs    (commit record)
 *   4. fsync
 *   5. write images to their final locations           (checkpoint)
 *   6. fsync
 *   7. write journal header: state=CLEAN
 *   8. fsync
 *
 * Crash before (4): header not committed -> txn discarded (rollback).
 * Crash after (4): replay applies images again (idempotent), then CLEAN.
 * A torn header write is detected by CRC and treated as uncommitted.
 *
 * Journal header block layout:
 *   0   8   magic "SLOPJRNL"
 *   8   8   seq
 *   16  4   state (0=CLEAN, 1=COMMITTED)
 *   20  4   nblocks
 *   24  4   images_crc (crc32 over all image blocks, in order)
 *   32  8*n target block numbers
 *   4092 4  header_crc (crc32 over bytes [0,4092))
 */
#include "fs.h"
#include "codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define JHDR_CRC_OFF (SFS_BLOCK_SIZE - 4)

/* deterministic crash injection for recovery tests:
 * SLOPFS_CRASH=intent  die after intent log, before commit record
 * SLOPFS_CRASH=commit  die after commit record, before checkpoint */
#include <unistd.h>
static void crash_point(const char *name)
{
    const char *want = getenv("SLOPFS_CRASH");
    if (want && strcmp(want, name) == 0)
        _exit(42);
}

int sfs_bread(sfs_fs_t *fs, uint64_t blk, void *buf)
{
    if (sfs_in_txn(fs)) {
        for (unsigned i = 0; i < fs->txn_n; i++) {
            if (fs->txn[i].blk == blk) {
                memcpy(buf, fs->txn[i].data, SFS_BLOCK_SIZE);
                return 0;
            }
        }
    }
    return sfs_dev_read(&fs->dev, blk, buf);
}

int sfs_bstage(sfs_fs_t *fs, uint64_t blk, const void *buf)
{
    assert(sfs_in_txn(fs));
    for (unsigned i = 0; i < fs->txn_n; i++) {
        if (fs->txn[i].blk == blk) {
            memcpy(fs->txn[i].data, buf, SFS_BLOCK_SIZE);
            return 0;
        }
    }
    if (fs->txn_n >= SFS_TXN_MAX_BLOCKS)
        return -ENOSPC;
    fs->txn[fs->txn_n].blk = blk;
    memcpy(fs->txn[fs->txn_n].data, buf, SFS_BLOCK_SIZE);
    fs->txn_n++;
    return 0;
}

int sfs_bput(sfs_fs_t *fs, uint64_t blk, const void *buf)
{
    /* free in the committed bitmap = unreferenced by committed metadata:
     * the pre-commit-record fsync makes the direct write durable in
     * order, and a rollback simply never references the block */
    if (!((fs->bmap_disk[blk >> 3] >> (blk & 7)) & 1))
        return sfs_dev_write(&fs->dev, blk, buf);
    return sfs_bstage(fs, blk, buf);
}

int sfs_txn_begin(sfs_fs_t *fs)
{
    if (fs->readonly)
        return -EROFS;
    if (fs->txn_depth == 0) {   /* inner begins join the outer txn */
        fs->txn_n = 0;
        fs->txn_freed_n = 0;
    }
    fs->txn_depth++;
    return 0;
}

void sfs_txn_abort(sfs_fs_t *fs)
{
    if (fs->txn_depth == 0)
        return;
    fs->txn_depth = 0;   /* abort discards the whole nested txn */
    fs->txn_n = 0;
    fs->txn_freed_n = 0;
    /* staged bitmap/superblock mutations also touched the in-memory
     * caches; reload them from the (unchanged) disk state */
    sfs_reload_state(fs);
}

static void jhdr_encode(uint8_t *buf, uint64_t seq, uint32_t state,
                        uint32_t nblocks, uint32_t images_crc,
                        const sfs_txn_entry_t *txn)
{
    memset(buf, 0, SFS_BLOCK_SIZE);
    memcpy(buf, SFS_JMAGIC, SFS_JMAGIC_LEN);
    put_le64(buf + 8, seq);
    put_le32(buf + 16, state);
    put_le32(buf + 20, nblocks);
    put_le32(buf + 24, images_crc);
    for (uint32_t i = 0; i < nblocks; i++)
        put_le64(buf + 32 + 8 * i, txn ? txn[i].blk : 0);
    put_le32(buf + JHDR_CRC_OFF, sfs_crc32(0, buf, JHDR_CRC_OFF));
}

static int jhdr_write(sfs_fs_t *fs, const uint8_t *buf)
{
    int rc = sfs_dev_write(&fs->dev, fs->sb.journal_start, buf);
    if (rc) return rc;
    return sfs_dev_sync(&fs->dev);
}

int sfs_txn_commit(sfs_fs_t *fs)
{
    assert(sfs_in_txn(fs));
    if (fs->txn_depth > 1) {   /* inner commit: stay grouped in outer txn */
        fs->txn_depth--;
        return 0;
    }
    if (fs->txn_n == 0) {      /* nothing staged: no-op commit */
        fs->txn_depth = 0;
        fs->txn_freed_n = 0;
        return 0;
    }
    int rc = 0;
    uint8_t hdr[SFS_BLOCK_SIZE];
    uint8_t sbbuf[SFS_BLOCK_SIZE];

    /* every txn carries the superblock: free counts + generation bump
     * (the generation counter lets slopfs-view detect changes) */
    fs->sb.generation++;
    sfs_super_encode(&fs->sb, sbbuf);
    rc = sfs_bstage(fs, 0, sbbuf);
    if (rc) goto fail;

    if (fs->txn_n + 1 > fs->sb.journal_blocks) { rc = -ENOSPC; goto fail; }

    /* 1-2: intent log */
    uint32_t images_crc = 0;
    for (unsigned i = 0; i < fs->txn_n; i++) {
        rc = sfs_dev_write(&fs->dev, fs->sb.journal_start + 1 + i,
                           fs->txn[i].data);
        if (rc) goto fail;
        images_crc = sfs_crc32(images_crc, fs->txn[i].data, SFS_BLOCK_SIZE);
    }
    rc = sfs_dev_sync(&fs->dev);
    if (rc) goto fail;
    crash_point("intent");

    /* 3-4: commit record */
    jhdr_encode(hdr, fs->sb.generation, SFS_JSTATE_COMMITTED,
                fs->txn_n, images_crc, fs->txn);
    rc = jhdr_write(fs, hdr);
    if (rc) goto fail;
    crash_point("commit");

    /* 5-6: checkpoint in place */
    for (unsigned i = 0; i < fs->txn_n; i++) {
        rc = sfs_dev_write(&fs->dev, fs->txn[i].blk, fs->txn[i].data);
        if (rc) goto fail;   /* journal still committed: recoverable */
    }
    rc = sfs_dev_sync(&fs->dev);
    if (rc) goto fail;

    /* 7-8: mark clean (nblocks retained for stats: last commit size;
     * recovery ignores it in CLEAN state) */
    jhdr_encode(hdr, fs->sb.generation, SFS_JSTATE_CLEAN, fs->txn_n, 0, NULL);
    rc = jhdr_write(fs, hdr);
    if (rc) goto fail;

    fs->txn_depth = 0;
    fs->txn_n = 0;
    sfs_alloc_commit_hook(fs);   /* freed extents re-enter the run list */
    memcpy(fs->bmap_disk, fs->bmap,
           (size_t)fs->sb.bitmap_blocks * SFS_BLOCK_SIZE);

    /* debug invariant system: verify the whole filesystem after every
     * committed transaction */
    if (!fs->in_check && getenv("SLOPFS_DEBUG_INVARIANTS")) {
        int problems = sfs_check(fs, 1, 0, 0);
        if (problems != 0) {
            fprintf(stderr,
                    "slopfs: INVARIANT VIOLATION after commit gen=%llu "
                    "(%d problems)\n",
                    (unsigned long long)fs->sb.generation, problems);
            abort();
        }
    }
    return 0;

fail:
    sfs_txn_abort(fs);
    return rc;
}

int sfs_journal_recover(sfs_fs_t *fs)
{
    /* read-only mounts (slopfs-view) must never write: skip recovery and
     * tolerate a possibly not-yet-checkpointed committed txn */
    if (fs->readonly)
        return 0;

    uint8_t hdr[SFS_BLOCK_SIZE];
    int rc = sfs_dev_read(&fs->dev, fs->sb.journal_start, hdr);
    if (rc) return rc;

    int valid = memcmp(hdr, SFS_JMAGIC, SFS_JMAGIC_LEN) == 0 &&
                get_le32(hdr + JHDR_CRC_OFF) ==
                    sfs_crc32(0, hdr, JHDR_CRC_OFF);

    /* already clean and well-formed: leave it alone (the CLEAN header
     * retains the last commit's nblocks for stats) */
    if (valid && get_le32(hdr + 16) == SFS_JSTATE_CLEAN)
        return 0;

    uint32_t replayed = 0;
    if (valid && get_le32(hdr + 16) == SFS_JSTATE_COMMITTED) {
        uint32_t n = get_le32(hdr + 20);
        uint32_t want_crc = get_le32(hdr + 24);
        if (n > SFS_TXN_MAX_BLOCKS || n + 1 > fs->sb.journal_blocks)
            valid = 0;
        if (valid) {
            uint8_t *images = malloc((size_t)n * SFS_BLOCK_SIZE);
            if (!images) return -ENOMEM;
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n && rc == 0; i++) {
                rc = sfs_dev_read(&fs->dev, fs->sb.journal_start + 1 + i,
                                  images + (size_t)i * SFS_BLOCK_SIZE);
                if (rc == 0)
                    crc = sfs_crc32(crc,
                                    images + (size_t)i * SFS_BLOCK_SIZE,
                                    SFS_BLOCK_SIZE);
            }
            if (rc == 0 && crc == want_crc) {
                /* replay committed transaction (idempotent) */
                for (uint32_t i = 0; i < n && rc == 0; i++)
                    rc = sfs_dev_write(&fs->dev, get_le64(hdr + 32 + 8 * i),
                                       images + (size_t)i * SFS_BLOCK_SIZE);
                if (rc == 0)
                    rc = sfs_dev_sync(&fs->dev);
                if (rc == 0)
                    replayed = n;
            }
            /* crc mismatch on images of a committed record cannot happen
             * under the protocol (images are synced before the commit
             * record); treat defensively as uncommitted */
            free(images);
            if (rc) return rc;
        }
    }

    /* roll back / reset: leave journal clean (nblocks records the size
     * of the replayed txn, 0 if rolled back) */
    jhdr_encode(hdr, valid ? get_le64(hdr + 8) : 0, SFS_JSTATE_CLEAN,
                replayed, 0, NULL);
    return jhdr_write(fs, hdr);
}
