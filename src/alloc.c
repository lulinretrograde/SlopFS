/*
 * Extent-aware allocation.
 *
 * The journaled on-disk bitmap remains the authoritative allocation
 * state (it is what fsck and recovery trust). On top of it the mount
 * keeps an in-memory free extent list: coalesced free runs sorted by
 * start. Allocation is best-fit over the runs (smallest run that
 * satisfies the request, lowest start as tie-break, which also makes
 * the allocator deterministic for replay mode); frees are deferred to
 * commit time before re-entering the run list, because the freed
 * blocks may still be referenced by the last committed metadata.
 */
#include "fs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define BITS_PER_BLK (SFS_BLOCK_SIZE * 8u)

static int bit_get(const uint8_t *map, uint64_t idx)
{
    return (map[idx >> 3] >> (idx & 7)) & 1;
}
static void bit_set(uint8_t *map, uint64_t idx)
{
    map[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}
static void bit_clear(uint8_t *map, uint64_t idx)
{
    map[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

/* flip a whole run in the cached bitmap and stage the touched bitmap
 * blocks into the current transaction */
static int run_mark(sfs_fs_t *fs, uint32_t start, uint32_t count, int used)
{
    for (uint32_t i = 0; i < count; i++) {
        assert(bit_get(fs->bmap, start + i) != used); /* no double alloc/free */
        if (used) bit_set(fs->bmap, start + i);
        else      bit_clear(fs->bmap, start + i);
    }
    uint64_t mb0 = start / BITS_PER_BLK;
    uint64_t mb1 = (start + count - 1) / BITS_PER_BLK;
    for (uint64_t mb = mb0; mb <= mb1; mb++) {
        int rc = sfs_bstage(fs, fs->sb.bitmap_start + mb,
                            fs->bmap + mb * SFS_BLOCK_SIZE);
        if (rc) return rc;
    }
    return 0;
}

/* ------------------------------------------------------- free run list */

static int runs_reserve(sfs_fs_t *fs, uint32_t need)
{
    if (fs->nruns + need <= fs->runs_cap)
        return 0;
    uint32_t cap = fs->runs_cap ? fs->runs_cap * 2 : 256;
    while (cap < fs->nruns + need) cap *= 2;
    sfs_run_t *nr = realloc(fs->runs, (size_t)cap * sizeof(*nr));
    if (!nr) return -ENOMEM;
    fs->runs = nr;
    fs->runs_cap = cap;
    return 0;
}

/* index of first run with start >= s */
static uint32_t runs_lower_bound(sfs_fs_t *fs, uint32_t s)
{
    uint32_t lo = 0, hi = fs->nruns;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (fs->runs[mid].start < s) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* insert a free run, coalescing with adjacent neighbours */
static int runs_insert(sfs_fs_t *fs, uint32_t start, uint32_t len)
{
    uint32_t i = runs_lower_bound(fs, start);
    int merge_prev = i > 0 &&
        fs->runs[i - 1].start + fs->runs[i - 1].len == start;
    int merge_next = i < fs->nruns && start + len == fs->runs[i].start;

    if (merge_prev && merge_next) {
        fs->runs[i - 1].len += len + fs->runs[i].len;
        memmove(&fs->runs[i], &fs->runs[i + 1],
                (size_t)(fs->nruns - i - 1) * sizeof(sfs_run_t));
        fs->nruns--;
    } else if (merge_prev) {
        fs->runs[i - 1].len += len;
    } else if (merge_next) {
        fs->runs[i].start = start;
        fs->runs[i].len += len;
    } else {
        int rc = runs_reserve(fs, 1);
        if (rc) return rc;
        memmove(&fs->runs[i + 1], &fs->runs[i],
                (size_t)(fs->nruns - i) * sizeof(sfs_run_t));
        fs->runs[i].start = start;
        fs->runs[i].len = len;
        fs->nruns++;
    }
    return 0;
}

/* take `take` blocks from the front of run i (offset 0) or carve at an
 * exact position equal to the run start */
static void runs_take_front(sfs_fs_t *fs, uint32_t i, uint32_t take)
{
    if (take == fs->runs[i].len) {
        memmove(&fs->runs[i], &fs->runs[i + 1],
                (size_t)(fs->nruns - i - 1) * sizeof(sfs_run_t));
        fs->nruns--;
    } else {
        fs->runs[i].start += take;
        fs->runs[i].len -= take;
    }
}

int sfs_runs_rebuild(sfs_fs_t *fs)
{
    fs->nruns = 0;
    uint64_t b = fs->sb.data_start;
    while (b < fs->sb.total_blocks) {
        if (bit_get(fs->bmap, b)) { b++; continue; }
        uint64_t s = b;
        while (b < fs->sb.total_blocks && !bit_get(fs->bmap, b))
            b++;
        int rc = runs_reserve(fs, 1);
        if (rc) return rc;
        fs->runs[fs->nruns].start = (uint32_t)s;
        fs->runs[fs->nruns].len = (uint32_t)(b - s);
        fs->nruns++;
    }
    return 0;
}

uint32_t sfs_largest_free_run(sfs_fs_t *fs)
{
    uint32_t best = 0;
    for (uint32_t i = 0; i < fs->nruns; i++)
        if (fs->runs[i].len > best)
            best = fs->runs[i].len;
    return best;
}

/* --------------------------------------------------------- allocation */

int sfs_ext_alloc(sfs_fs_t *fs, uint32_t want, sfs_extent_t *out)
{
    assert(sfs_in_txn(fs));
    if (want == 0 || fs->nruns == 0)
        return -ENOSPC;

    /* best fit: smallest run >= want; fall back to the largest run.
     * lowest start wins ties (determinism). */
    uint32_t best_fit = UINT32_MAX, best_fit_i = 0;
    uint32_t largest = 0, largest_i = 0;
    for (uint32_t i = 0; i < fs->nruns; i++) {
        uint32_t len = fs->runs[i].len;
        if (len >= want && len < best_fit) {
            best_fit = len;
            best_fit_i = i;
        }
        if (len > largest) {
            largest = len;
            largest_i = i;
        }
    }

    uint32_t i = best_fit != UINT32_MAX ? best_fit_i : largest_i;
    uint32_t got = want < fs->runs[i].len ? want : fs->runs[i].len;
    uint32_t start = fs->runs[i].start;

    int rc = run_mark(fs, start, got, 1);
    if (rc) return rc;
    runs_take_front(fs, i, got);
    fs->sb.free_blocks -= got;
    out->start = start;
    out->count = got;
    return 0;
}

uint32_t sfs_ext_alloc_at(sfs_fs_t *fs, uint32_t start, uint32_t want)
{
    assert(sfs_in_txn(fs));
    if (want == 0)
        return 0;
    uint32_t i = runs_lower_bound(fs, start);
    if (i >= fs->nruns || fs->runs[i].start != start)
        return 0;   /* the target block is not the head of a free run */
    uint32_t got = want < fs->runs[i].len ? want : fs->runs[i].len;
    if (run_mark(fs, start, got, 1))
        return 0;
    runs_take_front(fs, i, got);
    fs->sb.free_blocks -= got;
    return got;
}

int sfs_ext_free(sfs_fs_t *fs, uint32_t start, uint32_t count)
{
    assert(sfs_in_txn(fs));
    if (count == 0)
        return 0;
    if (start < fs->sb.data_start ||
        (uint64_t)start + count > fs->sb.total_blocks)
        return -EINVAL;
    for (uint32_t i = 0; i < count; i++)
        if (!bit_get(fs->bmap, start + i))
            return -EINVAL;   /* double free */

    int rc = run_mark(fs, start, count, 0);
    if (rc) return rc;
    fs->sb.free_blocks += count;

    /* defer run-list insertion to commit */
    if (fs->txn_freed_n == fs->txn_freed_cap) {
        unsigned cap = fs->txn_freed_cap ? fs->txn_freed_cap * 2 : 64;
        sfs_run_t *nf = realloc(fs->txn_freed, cap * sizeof(*nf));
        if (!nf) return -ENOMEM;
        fs->txn_freed = nf;
        fs->txn_freed_cap = cap;
    }
    fs->txn_freed[fs->txn_freed_n].start = start;
    fs->txn_freed[fs->txn_freed_n].len = count;
    fs->txn_freed_n++;
    return 0;
}

void sfs_alloc_commit_hook(sfs_fs_t *fs)
{
    for (unsigned i = 0; i < fs->txn_freed_n; i++)
        runs_insert(fs, fs->txn_freed[i].start, fs->txn_freed[i].len);
    fs->txn_freed_n = 0;
}

/* ------------------------------------------------------------- inodes */

static int stage_imap_blk(sfs_fs_t *fs, uint32_t ino)
{
    uint64_t mb = ino / BITS_PER_BLK;
    return sfs_bstage(fs, fs->sb.ibitmap_start + mb,
                      fs->imap + mb * SFS_BLOCK_SIZE);
}

int sfs_ialloc(sfs_fs_t *fs, uint32_t *out)
{
    assert(sfs_in_txn(fs));
    if (fs->sb.free_inodes == 0)
        return -ENOSPC;
    uint32_t total = fs->sb.inode_count;
    uint32_t start = fs->ialloc_hint >= total ? 1 : fs->ialloc_hint;
    for (uint32_t scanned = 0; scanned < total; scanned++) {
        uint32_t ino = start + scanned;
        if (ino >= total)
            ino = 1 + (ino - total);
        if (ino == 0)
            continue;
        if (!bit_get(fs->imap, ino)) {
            bit_set(fs->imap, ino);
            int rc = stage_imap_blk(fs, ino);
            if (rc) { bit_clear(fs->imap, ino); return rc; }
            fs->sb.free_inodes--;
            fs->ialloc_hint = ino + 1;
            *out = ino;
            return 0;
        }
    }
    return -ENOSPC;
}

int sfs_ifree(sfs_fs_t *fs, uint32_t ino)
{
    assert(sfs_in_txn(fs));
    if (ino == 0 || ino >= fs->sb.inode_count)
        return -EINVAL;
    if (!bit_get(fs->imap, ino))
        return -EINVAL;
    bit_clear(fs->imap, ino);
    int rc = stage_imap_blk(fs, ino);
    if (rc) { bit_set(fs->imap, ino); return rc; }
    fs->sb.free_inodes++;
    return 0;
}
