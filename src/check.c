/*
 * Integrity checker (shared by slopfs-fsck and the post-commit
 * invariant hook).
 *
 * Verified invariants:
 *   - extents are in range and never overlap (incl. extent-overflow and
 *     directory blocks)
 *   - file size matches the extent sum; dir size matches exactly
 *   - every allocated bitmap block is referenced by exactly one inode
 *     (no orphan blocks), every referenced block is allocated
 *   - inode bitmap matches inode table types
 *   - superblock free counts match the bitmaps
 *   - journal header is valid and clean
 *   - deep: directory hash chains are acyclic, complete, bucket-correct;
 *     entries reference valid allocated inodes; link counts match
 *
 * Repair (opt-in): rebuilds the block/inode bitmaps from the reference
 * map, fixes free counts, removes invalid directory entries, and
 * rebuilds the in-memory free extent list.
 */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    uint32_t dir_ino;
    char name[SFS_NAME_MAX + 1];
} bad_dirent_t;

typedef struct {
    sfs_fs_t *fs;
    uint8_t  *ref;       /* reference block bitmap built from metadata */
    uint8_t  *types;     /* per-inode type from the table */
    uint16_t *nlinks;
    uint32_t *dref;      /* dirent references per inode */
    bad_dirent_t *bad;   /* invalid entries collected for repair */
    uint32_t nbad, badcap;
    int problems;
    int verbose;
} chk_t;

static void prob(chk_t *c, const char *fmt, ...)
{
    c->problems++;
    if (c->verbose) {
        va_list ap;
        va_start(ap, fmt);
        fputs("fsck: ", stdout);
        vprintf(fmt, ap);
        putchar('\n');
        va_end(ap);
    }
}

static void mark_run(chk_t *c, uint32_t ino, uint32_t start, uint32_t count)
{
    sfs_fs_t *fs = c->fs;
    if (start < fs->sb.data_start ||
        (uint64_t)start + count > fs->sb.total_blocks) {
        prob(c, "ino %u: extent [%u+%u] out of data range", ino, start,
             count);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (bit_get(c->ref, start + i))
            prob(c, "ino %u: block %u multiply referenced (overlap)",
                 ino, start + i);
        else
            bit_set(c->ref, start + i);
    }
}

/* ----------------------------------------------------- pass 1: inodes */

static int check_inodes(chk_t *c)
{
    sfs_fs_t *fs = c->fs;
    uint8_t buf[SFS_BLOCK_SIZE];
    for (uint64_t b = 0; b < fs->sb.inode_blocks; b++) {
        int rc = sfs_bread(fs, fs->sb.inode_start + b, buf);
        if (rc) return rc;
        for (unsigned i = 0; i < SFS_INODES_PER_BLK; i++) {
            uint32_t ino = (uint32_t)(b * SFS_INODES_PER_BLK + i);
            if (ino == 0 || ino >= fs->sb.inode_count)
                continue;
            sfs_inode_t in;
            sfs_inode_decode(&in, buf + i * SFS_INODE_SIZE);
            int alloc = bit_get(fs->imap, ino);
            if (in.type == SFS_TYPE_FREE) {
                if (alloc)
                    prob(c, "ino %u: allocated in bitmap but free in "
                            "table", ino);
                continue;
            }
            if (!alloc)
                prob(c, "ino %u: in use but free in inode bitmap", ino);
            if (in.type != SFS_TYPE_FILE && in.type != SFS_TYPE_DIR) {
                prob(c, "ino %u: invalid type %u", ino, in.type);
                continue;
            }
            c->types[ino] = (uint8_t)in.type;
            c->nlinks[ino] = in.nlinks;
            if (!(in.flags & SFS_IFLAG_EXTENTS))
                prob(c, "ino %u: not extent-mapped in a v2 filesystem",
                     ino);
            if (in.nextents > SFS_MAX_EXTENTS) {
                prob(c, "ino %u: nextents %u exceeds limit", ino,
                     in.nextents);
                continue;
            }
            if (in.nextents > SFS_INLINE_EXTENTS && in.ext_block == 0) {
                prob(c, "ino %u: overflow extents without ext_block", ino);
                continue;
            }
            uint64_t sum = 0;
            for (uint32_t x = 0; x < in.nextents; x++) {
                sfs_extent_t e;
                rc = sfs_ext_get(fs, &in, x, &e);
                if (rc) return rc;
                if (e.count == 0) {
                    prob(c, "ino %u: zero-length extent %u", ino, x);
                    continue;
                }
                mark_run(c, ino, e.start, e.count);
                sum += e.count;
            }
            if (in.ext_block)
                mark_run(c, ino, in.ext_block, 1);
            uint64_t need = (in.size + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
            if (in.type == SFS_TYPE_FILE && need != sum)
                prob(c, "ino %u: size %llu needs %llu blocks, extents "
                        "hold %llu", ino, (unsigned long long)in.size,
                     (unsigned long long)need, (unsigned long long)sum);
            if (in.type == SFS_TYPE_DIR &&
                (in.size != sum * SFS_BLOCK_SIZE || sum < 1))
                prob(c, "ino %u: dir size %llu inconsistent with %llu "
                        "blocks", ino, (unsigned long long)in.size,
                     (unsigned long long)sum);
        }
    }
    return 0;
}

/* ----------------------------------------- pass 2: directories (deep) */

static int dir_slot_read(sfs_fs_t *fs, const sfs_inode_t *dir, uint32_t s,
                         sfs_dirent_t *de)
{
    uint64_t phys;
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = sfs_inode_lookup_blk(fs, dir, 1 + s / SFS_DIRENTS_PER_BLK,
                                  &phys);
    if (rc) return rc;
    if (phys == 0) return -EIO;
    rc = sfs_bread(fs, phys, buf);
    if (rc) return rc;
    sfs_dirent_decode(de, buf + (s % SFS_DIRENTS_PER_BLK) * SFS_DIRENT_SIZE);
    return 0;
}

static void note_bad(chk_t *c, uint32_t dir_ino, const char *name)
{
    if (c->nbad == c->badcap) {
        uint32_t cap = c->badcap ? c->badcap * 2 : 16;
        bad_dirent_t *nb = realloc(c->bad, (size_t)cap * sizeof(*nb));
        if (!nb) return;   /* repair list best-effort */
        c->bad = nb;
        c->badcap = cap;
    }
    c->bad[c->nbad].dir_ino = dir_ino;
    strcpy(c->bad[c->nbad].name, name);
    c->nbad++;
}

static int check_dir(chk_t *c, uint32_t dir_ino)
{
    sfs_fs_t *fs = c->fs;
    sfs_inode_t dir;
    int rc = sfs_iget(fs, dir_ino, &dir);
    if (rc) return rc;
    if (dir.size < SFS_BLOCK_SIZE)
        return 0;   /* already reported in pass 1 */

    uint32_t nslots = (uint32_t)(dir.size / SFS_BLOCK_SIZE - 1)
                      * SFS_DIRENTS_PER_BLK;
    uint8_t *seen = calloc(nslots / 8 + 1, 1);
    if (!seen) return -ENOMEM;

    uint64_t bphys;
    uint8_t bb[SFS_BLOCK_SIZE];
    rc = sfs_inode_lookup_blk(fs, &dir, 0, &bphys);
    if (rc == 0 && bphys == 0) rc = -EIO;
    if (rc == 0) rc = sfs_bread(fs, bphys, bb);
    if (rc) { free(seen); return rc; }

    for (uint32_t bkt = 0; bkt < SFS_DIR_NBUCKETS; bkt++) {
        uint32_t slot = get_le32(bb + bkt * 4);
        uint32_t hops = 0;
        while (slot != SFS_DIR_EOC) {
            if (slot >= nslots) {
                prob(c, "dir %u: bucket %u chain leaves slot range "
                        "(slot %u)", dir_ino, bkt, slot);
                break;
            }
            if (++hops > nslots) {
                prob(c, "dir %u: bucket %u chain cycles", dir_ino, bkt);
                break;
            }
            if (bit_get(seen, slot)) {
                prob(c, "dir %u: slot %u on multiple chains", dir_ino,
                     slot);
                break;
            }
            bit_set(seen, slot);
            sfs_dirent_t de;
            rc = dir_slot_read(fs, &dir, slot, &de);
            if (rc) { free(seen); return rc; }
            if (de.ino == 0) {
                prob(c, "dir %u: empty slot %u linked in bucket %u",
                     dir_ino, slot, bkt);
            } else {
                if (sfs_name_hash(de.name) % SFS_DIR_NBUCKETS != bkt)
                    prob(c, "dir %u: entry '%s' chained in wrong bucket "
                            "%u", dir_ino, de.name, bkt);
                if (de.ino >= fs->sb.inode_count ||
                    c->types[de.ino] == SFS_TYPE_FREE) {
                    prob(c, "dir %u: entry '%s' references invalid inode "
                            "%u", dir_ino, de.name, de.ino);
                    note_bad(c, dir_ino, de.name);
                } else {
                    c->dref[de.ino]++;
                }
            }
            slot = de.next;
        }
    }

    /* every live slot must be reachable from its bucket */
    for (uint32_t s = 0; s < nslots; s++) {
        if (bit_get(seen, s))
            continue;
        sfs_dirent_t de;
        rc = dir_slot_read(fs, &dir, s, &de);
        if (rc) { free(seen); return rc; }
        if (de.ino != 0)
            prob(c, "dir %u: live slot %u ('%s') unreachable from hash "
                    "index", dir_ino, s, de.name);
    }
    free(seen);
    return 0;
}

/* ------------------------------------------------------------- repair */

static int apply_repairs(chk_t *c, const uint8_t *want_imap)
{
    sfs_fs_t *fs = c->fs;
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;

    /* remove invalid directory entries */
    for (uint32_t i = 0; i < c->nbad; i++) {
        sfs_inode_t dir;
        rc = sfs_iget(fs, c->bad[i].dir_ino, &dir);
        if (rc == 0)
            rc = sfs_dir_remove(fs, c->bad[i].dir_ino, &dir,
                                c->bad[i].name);
        if (rc == 0)
            rc = sfs_iput(fs, c->bad[i].dir_ino, &dir);
        if (rc) { sfs_txn_abort(fs); return rc; }
    }

    /* rebuild block bitmap from the reference map (keep the padding
     * bits beyond total_blocks as-is) */
    uint64_t freeb = 0;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; b++) {
        if (bit_get(c->ref, b)) bit_set(fs->bmap, b);
        else { bit_clear(fs->bmap, b); freeb++; }
    }
    for (uint64_t i = 0; i < fs->sb.bitmap_blocks; i++) {
        rc = sfs_bstage(fs, fs->sb.bitmap_start + i,
                        fs->bmap + i * SFS_BLOCK_SIZE);
        if (rc) { sfs_txn_abort(fs); return rc; }
    }
    fs->sb.free_blocks = freeb;

    /* rebuild inode bitmap from the table */
    uint64_t freei = 0;
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (bit_get(want_imap, ino)) bit_set(fs->imap, ino);
        else { bit_clear(fs->imap, ino); freei++; }
    }
    for (uint64_t i = 0; i < fs->sb.ibitmap_blocks; i++) {
        rc = sfs_bstage(fs, fs->sb.ibitmap_start + i,
                        fs->imap + i * SFS_BLOCK_SIZE);
        if (rc) { sfs_txn_abort(fs); return rc; }
    }
    fs->sb.free_inodes = freei;

    rc = sfs_txn_commit(fs);
    if (rc) return rc;
    return sfs_runs_rebuild(fs);   /* free extent map from fixed bitmap */
}

/* --------------------------------------------------------------- main */

int sfs_check(sfs_fs_t *fs, int deep, int repair, int verbose)
{
    chk_t c;
    memset(&c, 0, sizeof(c));
    c.fs = fs;
    c.verbose = verbose;
    fs->in_check = 1;

    size_t bsz = (size_t)fs->sb.bitmap_blocks * SFS_BLOCK_SIZE;
    c.ref = calloc(bsz, 1);
    c.types = calloc(fs->sb.inode_count, 1);
    c.nlinks = calloc(fs->sb.inode_count, sizeof(uint16_t));
    c.dref = calloc(fs->sb.inode_count, sizeof(uint32_t));
    int rc = (!c.ref || !c.types || !c.nlinks || !c.dref) ? -ENOMEM : 0;
    if (rc) goto out;

    for (uint64_t b = 0; b < fs->sb.data_start; b++)
        bit_set(c.ref, b);

    rc = check_inodes(&c);
    if (rc) goto out;

    if (c.types[fs->sb.root_ino] != SFS_TYPE_DIR)
        prob(&c, "root inode %u is not a directory", fs->sb.root_ino);

    if (deep) {
        for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
            if (c.types[ino] != SFS_TYPE_DIR)
                continue;
            rc = check_dir(&c, ino);
            if (rc) goto out;
        }
        for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
            if (c.types[ino] == SFS_TYPE_FREE)
                continue;
            uint32_t want = ino == fs->sb.root_ino ? 0 : c.nlinks[ino];
            if (c.dref[ino] != want)
                prob(&c, "ino %u: referenced by %u entries, nlinks %u",
                     ino, c.dref[ino], c.nlinks[ino]);
        }
    }

    /* bitmap vs extent map */
    uint64_t freeb = 0;
    for (uint64_t b = fs->sb.data_start; b < fs->sb.total_blocks; b++) {
        int disk = bit_get(fs->bmap, b);
        int want = bit_get(c.ref, b);
        if (!disk) freeb++;
        if (disk && !want)
            prob(&c, "block %llu: allocated but unreferenced (orphan)",
                 (unsigned long long)b);
        else if (!disk && want)
            prob(&c, "block %llu: referenced but free in bitmap",
                 (unsigned long long)b);
    }
    if (freeb != fs->sb.free_blocks)
        prob(&c, "superblock free_blocks %llu, bitmap says %llu",
             (unsigned long long)fs->sb.free_blocks,
             (unsigned long long)freeb);

    /* inode bitmap counts */
    uint8_t *want_imap = calloc((size_t)fs->sb.ibitmap_blocks *
                                SFS_BLOCK_SIZE, 1);
    if (!want_imap) { rc = -ENOMEM; goto out; }
    uint64_t freei = 0;
    for (uint32_t ino = 1; ino < fs->sb.inode_count; ino++) {
        if (c.types[ino] != SFS_TYPE_FREE) bit_set(want_imap, ino);
        else freei++;
    }
    if (freei != fs->sb.free_inodes)
        prob(&c, "superblock free_inodes %llu, table says %llu",
             (unsigned long long)fs->sb.free_inodes,
             (unsigned long long)freei);

    /* journal header */
    {
        uint8_t hdr[SFS_BLOCK_SIZE];
        rc = sfs_dev_read(&fs->dev, fs->sb.journal_start, hdr);
        if (rc) { free(want_imap); goto out; }
        int valid = memcmp(hdr, SFS_JMAGIC, SFS_JMAGIC_LEN) == 0 &&
                    get_le32(hdr + SFS_BLOCK_SIZE - 4) ==
                        sfs_crc32(0, hdr, SFS_BLOCK_SIZE - 4);
        if (!valid)
            prob(&c, "journal header invalid (torn or corrupt)");
        else if (get_le32(hdr + 16) != SFS_JSTATE_CLEAN)
            prob(&c, "journal not clean (committed txn pending replay)");
    }

    if (repair && c.problems > 0 && !fs->readonly) {
        int fixed_from = c.problems;
        rc = apply_repairs(&c, want_imap);
        free(want_imap);
        if (rc) goto out;
        if (verbose)
            printf("fsck: repairs applied (%d problems), re-checking\n",
                   fixed_from);
        /* re-run to report the post-repair state */
        free(c.ref); free(c.types); free(c.nlinks); free(c.dref);
        free(c.bad);
        fs->in_check = 0;
        return sfs_check(fs, deep, 0, verbose);
    }
    free(want_imap);
    rc = 0;

out:
    free(c.ref); free(c.types); free(c.nlinks); free(c.dref); free(c.bad);
    fs->in_check = 0;
    return rc ? rc : c.problems;
}
