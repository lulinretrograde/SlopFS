/* SlopFS core: mount state, transactions, and high-level operations. */
#ifndef SLOPFS_FS_H
#define SLOPFS_FS_H

#include "format.h"
#include "blkdev.h"

typedef struct {
    uint64_t blk;
    uint8_t  data[SFS_BLOCK_SIZE];
} sfs_txn_entry_t;

/* a contiguous run of free blocks (in-memory free extent list) */
typedef struct {
    uint32_t start;
    uint32_t len;
} sfs_run_t;

typedef struct sfs_fs {
    sfs_dev_t dev;
    sfs_super_t sb;
    int readonly;
    int in_check;           /* suppress invariant-hook re-entry */
    uint64_t fake_now;      /* nonzero: deterministic clock (replay mode),
                             * incremented per timestamp request */

    /* cached allocation bitmaps (authoritative copy is on disk) */
    uint8_t *bmap;          /* block bitmap, bitmap_blocks * BLOCK_SIZE   */
    uint8_t *bmap_disk;     /* bitmap state as of last commit: a block
                             * free here is unreferenced by any committed
                             * metadata and safe to write directly        */
    uint8_t *imap;          /* inode bitmap, ibitmap_blocks * BLOCK_SIZE  */
    uint32_t ialloc_hint;

    /* free extent list: coalesced runs sorted by start, derived from the
     * bitmap at mount and maintained incrementally afterwards */
    sfs_run_t *runs;
    uint32_t nruns, runs_cap;

    /* current transaction: staged block images (write-ahead) */
    int txn_depth;          /* nested begins; commit happens at depth 0 */
    unsigned txn_n;
    sfs_txn_entry_t *txn;   /* SFS_TXN_MAX_BLOCKS entries */

    /* extents freed inside the current txn: excluded from allocation and
     * merged into the free run list only at commit (their on-disk
     * content may still be referenced by pre-txn metadata) */
    sfs_run_t *txn_freed;
    unsigned txn_freed_n, txn_freed_cap;
} sfs_fs_t;

#define sfs_in_txn(fs) ((fs)->txn_depth > 0)

uint64_t sfs_now(sfs_fs_t *fs);

/* staged-aware block I/O */
int sfs_bread(sfs_fs_t *fs, uint64_t blk, void *buf);
int sfs_bstage(sfs_fs_t *fs, uint64_t blk, const void *buf); /* into txn */
/* hybrid write: blocks free in the last committed bitmap are written
 * directly (unreachable until commit, so crash-safe and journal-free);
 * blocks referenced by committed metadata are staged through the txn */
int sfs_bput(sfs_fs_t *fs, uint64_t blk, const void *buf);

/* transactions (journal.c) — nestable: inner begin/commit pairs join the
 * outer transaction (grouped commit); abort discards the whole txn */
int sfs_txn_begin(sfs_fs_t *fs);
int sfs_txn_commit(sfs_fs_t *fs);
void sfs_txn_abort(sfs_fs_t *fs);
int sfs_journal_recover(sfs_fs_t *fs);

/* extent allocation (alloc.c)
 * sfs_ext_alloc: best-fit; may return fewer than want blocks (caller
 * loops). sfs_ext_alloc_at: extend in place, returns blocks obtained
 * starting exactly at `start` (0 if that block is unavailable). */
int sfs_ext_alloc(sfs_fs_t *fs, uint32_t want, sfs_extent_t *out);
uint32_t sfs_ext_alloc_at(sfs_fs_t *fs, uint32_t start, uint32_t want);
int sfs_ext_free(sfs_fs_t *fs, uint32_t start, uint32_t count);
void sfs_alloc_commit_hook(sfs_fs_t *fs);   /* merge txn frees into runs */
int sfs_runs_rebuild(sfs_fs_t *fs);         /* derive runs from bitmap */
uint32_t sfs_largest_free_run(sfs_fs_t *fs);
int sfs_ialloc(sfs_fs_t *fs, uint32_t *out);
int sfs_ifree(sfs_fs_t *fs, uint32_t ino);

/* inodes & extents (inode.c) */
int sfs_iget(sfs_fs_t *fs, uint32_t ino, sfs_inode_t *out);
int sfs_iput(sfs_fs_t *fs, uint32_t ino, const sfs_inode_t *in);
/* read extent idx (0..nextents-1), inline or overflow */
int sfs_ext_get(sfs_fs_t *fs, const sfs_inode_t *in, uint32_t idx,
                sfs_extent_t *out);
/* append an extent to the file map, merging with the last extent when
 * contiguous; allocates the overflow block on demand */
int sfs_ext_append(sfs_fs_t *fs, sfs_inode_t *in, sfs_extent_t e);
/* logical block -> physical block (0 if beyond mapping) */
int sfs_inode_lookup_blk(sfs_fs_t *fs, const sfs_inode_t *in,
                         uint32_t lblk, uint64_t *phys);
int sfs_inode_nblocks(sfs_fs_t *fs, const sfs_inode_t *in, uint32_t *out);
/* free mapped blocks beyond keep (splits the boundary extent) */
int sfs_inode_shrink(sfs_fs_t *fs, sfs_inode_t *in, uint32_t keep);
int sfs_inode_truncate(sfs_fs_t *fs, sfs_inode_t *in);

/* directories (dir.c) — hash-chained index, O(1) amortized lookup */
int sfs_dir_init(sfs_fs_t *fs, sfs_inode_t *dir);   /* alloc bucket block */
int sfs_dir_lookup(sfs_fs_t *fs, const sfs_inode_t *dir, const char *name,
                   uint32_t *ino_out);
int sfs_dir_add(sfs_fs_t *fs, uint32_t dir_ino, sfs_inode_t *dir,
                const char *name, uint32_t ino);
int sfs_dir_remove(sfs_fs_t *fs, uint32_t dir_ino, sfs_inode_t *dir,
                   const char *name);
int sfs_dir_is_empty(sfs_fs_t *fs, const sfs_inode_t *dir);
/* cb returns nonzero to stop iteration; slot order = stable ls order */
typedef int (*sfs_dir_cb)(const sfs_dirent_t *de, void *arg);
int sfs_dir_iterate(sfs_fs_t *fs, const sfs_inode_t *dir,
                    sfs_dir_cb cb, void *arg);

/* lifecycle + namespace ops (fs.c) */
int sfs_mkfs(const char *path, uint64_t nblocks);
/* re-read superblock + bitmap caches + free runs from disk */
int sfs_reload_state(sfs_fs_t *fs);
int sfs_mount(sfs_fs_t *fs, const char *path, int readonly);
void sfs_unmount(sfs_fs_t *fs);

int sfs_resolve(sfs_fs_t *fs, const char *path, uint32_t *ino_out);
int sfs_resolve_parent(sfs_fs_t *fs, const char *path, uint32_t *parent_out,
                       char *name_out /* SFS_NAME_MAX+1 */);

int sfs_mkdir(sfs_fs_t *fs, const char *path);
int sfs_creat(sfs_fs_t *fs, const char *path);
int sfs_write_file(sfs_fs_t *fs, const char *path, const void *data,
                   uint64_t len, int append);
int sfs_read_file(sfs_fs_t *fs, const char *path, uint8_t **out, uint64_t *len);
int sfs_unlink(sfs_fs_t *fs, const char *path);
int sfs_rename(sfs_fs_t *fs, const char *from, const char *to);
int sfs_stat_path(sfs_fs_t *fs, const char *path, uint32_t *ino_out,
                  sfs_inode_t *out);

/* migration (migrate.c) */
int sfs_migrate_v1(sfs_fs_t *fs);

/* integrity checking (check.c) — returns number of problems found
 * (after repairs, if repair set); negative on I/O error */
int sfs_check(sfs_fs_t *fs, int deep, int repair, int verbose);

/* deterministic replay (replay.c): create image at path and apply a
 * seed-derived workload; same seed => byte-identical image */
int sfs_replay(const char *path, uint64_t seed, uint32_t nops);

/* statistics (stats.c) */
typedef struct {
    uint64_t total_blocks, free_blocks, data_blocks;
    uint32_t largest_free_run;
    double   frag_ratio;          /* 1 - largest_free_run/free_blocks */
    double   avg_extents_per_file;
    uint32_t nfiles, ndirs;
    uint32_t inode_count;
    uint64_t free_inodes;
    uint64_t journal_blocks;
    uint32_t journal_last_txn;    /* image blocks in last commit record */
} sfs_stats_t;
int sfs_collect_stats(sfs_fs_t *fs, sfs_stats_t *st);

#endif
