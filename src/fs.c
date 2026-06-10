/* Filesystem lifecycle (mkfs/mount), path resolution, namespace ops,
 * and the v2 extent-based write paths. */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t sfs_now(sfs_fs_t *fs)
{
    if (fs->fake_now)
        return fs->fake_now++;
    return (uint64_t)time(NULL);
}

/* ------------------------------------------------------------------ mkfs */

int sfs_mkfs(const char *path, uint64_t nblocks)
{
    if (nblocks < 1024)
        return -EINVAL;

    sfs_super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.version = SFS_VERSION_V2;
    sb.block_size = SFS_BLOCK_SIZE;
    sb.total_blocks = nblocks;
    sb.root_ino = SFS_ROOT_INO;

    uint32_t icount = (uint32_t)(nblocks / 2);     /* 1 inode per 8 KiB */
    if (icount < 128) icount = 128;
    icount = (icount + SFS_INODES_PER_BLK - 1) / SFS_INODES_PER_BLK
             * SFS_INODES_PER_BLK;
    sb.inode_count = icount;

    uint64_t bits_per_blk = SFS_BLOCK_SIZE * 8ull;
    sb.bitmap_start   = 1;
    sb.bitmap_blocks  = (nblocks + bits_per_blk - 1) / bits_per_blk;
    sb.ibitmap_start  = sb.bitmap_start + sb.bitmap_blocks;
    sb.ibitmap_blocks = (icount + bits_per_blk - 1) / bits_per_blk;
    sb.inode_start    = sb.ibitmap_start + sb.ibitmap_blocks;
    sb.inode_blocks   = icount / SFS_INODES_PER_BLK;
    sb.journal_start  = sb.inode_start + sb.inode_blocks;
    sb.journal_blocks = SFS_TXN_MAX_BLOCKS + 4;
    sb.data_start     = sb.journal_start + sb.journal_blocks;
    if (sb.data_start + 16 > nblocks)
        return -EINVAL;        /* image too small for metadata */
    uint64_t rootblk = sb.data_start;   /* root dir bucket table */
    sb.free_blocks = nblocks - sb.data_start - 1;
    sb.free_inodes = icount - 2;               /* ino 0 reserved + root */
    sb.generation = 1;

    sfs_dev_t dev;
    int rc = sfs_dev_open(&dev, path, 1, nblocks);
    if (rc) return rc;

    uint8_t buf[SFS_BLOCK_SIZE];

    /* block bitmap: metadata region + root dir block used;
     * out-of-range bits marked used */
    uint8_t *bmap = calloc(sb.bitmap_blocks, SFS_BLOCK_SIZE);
    if (!bmap) { sfs_dev_close(&dev); return -ENOMEM; }
    for (uint64_t b = 0; b <= rootblk; b++)
        bmap[b >> 3] |= (uint8_t)(1u << (b & 7));
    for (uint64_t b = nblocks; b < sb.bitmap_blocks * bits_per_blk; b++)
        bmap[b >> 3] |= (uint8_t)(1u << (b & 7));
    for (uint64_t i = 0; i < sb.bitmap_blocks && rc == 0; i++)
        rc = sfs_dev_write(&dev, sb.bitmap_start + i,
                           bmap + i * SFS_BLOCK_SIZE);
    free(bmap);
    if (rc) { sfs_dev_close(&dev); return rc; }

    /* inode bitmap: ino 0 + root used; out-of-range bits used */
    uint8_t *imap = calloc(sb.ibitmap_blocks, SFS_BLOCK_SIZE);
    if (!imap) { sfs_dev_close(&dev); return -ENOMEM; }
    imap[0] |= 0x03;
    for (uint64_t i = icount; i < sb.ibitmap_blocks * bits_per_blk; i++)
        imap[i >> 3] |= (uint8_t)(1u << (i & 7));
    for (uint64_t i = 0; i < sb.ibitmap_blocks && rc == 0; i++)
        rc = sfs_dev_write(&dev, sb.ibitmap_start + i,
                           imap + i * SFS_BLOCK_SIZE);
    free(imap);
    if (rc) { sfs_dev_close(&dev); return rc; }

    /* root inode: extent-based dir, one block (the bucket table) */
    const char *ft = getenv("SLOPFS_FAKE_TIME");   /* deterministic mkfs */
    uint64_t now = ft ? strtoull(ft, NULL, 10) : (uint64_t)time(NULL);
    sfs_inode_t root;
    memset(&root, 0, sizeof(root));
    root.type = SFS_TYPE_DIR;
    root.nlinks = 1;
    root.flags = SFS_IFLAG_EXTENTS;
    root.size = SFS_BLOCK_SIZE;
    root.nextents = 1;
    root.ext[0].start = (uint32_t)rootblk;
    root.ext[0].count = 1;
    root.ctime = root.mtime = root.atime = now;
    memset(buf, 0, sizeof(buf));
    sfs_inode_encode(&root, buf + SFS_ROOT_INO * SFS_INODE_SIZE);
    rc = sfs_dev_write(&dev, sb.inode_start, buf);
    if (rc) { sfs_dev_close(&dev); return rc; }

    /* root dir bucket table: all buckets empty, free-slot hint 0 */
    for (unsigned b = 0; b < SFS_DIR_NBUCKETS; b++)
        put_le32(buf + b * 4, SFS_DIR_EOC);
    put_le32(buf + SFS_DIR_HINT_OFF, 0);
    rc = sfs_dev_write(&dev, rootblk, buf);
    if (rc) { sfs_dev_close(&dev); return rc; }

    /* clean journal header */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, SFS_JMAGIC, SFS_JMAGIC_LEN);
    /* seq=0, state=CLEAN, nblocks=0 */
    put_le32(buf + SFS_BLOCK_SIZE - 4,
             sfs_crc32(0, buf, SFS_BLOCK_SIZE - 4));
    rc = sfs_dev_write(&dev, sb.journal_start, buf);
    if (rc) { sfs_dev_close(&dev); return rc; }

    /* superblock last */
    sfs_super_encode(&sb, buf);
    rc = sfs_dev_write(&dev, 0, buf);
    if (rc == 0)
        rc = sfs_dev_sync(&dev);
    sfs_dev_close(&dev);
    return rc;
}

/* --------------------------------------------------------------- mount */

int sfs_reload_state(sfs_fs_t *fs)
{
    uint8_t buf[SFS_BLOCK_SIZE];
    int rc = sfs_dev_read(&fs->dev, 0, buf);
    if (rc) return rc;
    rc = sfs_super_decode(&fs->sb, buf);
    if (rc) return rc;

    size_t bsz = (size_t)fs->sb.bitmap_blocks * SFS_BLOCK_SIZE;
    size_t isz = (size_t)fs->sb.ibitmap_blocks * SFS_BLOCK_SIZE;
    if (!fs->bmap)      fs->bmap = malloc(bsz);
    if (!fs->bmap_disk) fs->bmap_disk = malloc(bsz);
    if (!fs->imap)      fs->imap = malloc(isz);
    if (!fs->bmap || !fs->bmap_disk || !fs->imap)
        return -ENOMEM;

    for (uint64_t i = 0; i < fs->sb.bitmap_blocks; i++) {
        rc = sfs_dev_read(&fs->dev, fs->sb.bitmap_start + i,
                          fs->bmap + i * SFS_BLOCK_SIZE);
        if (rc) return rc;
    }
    memcpy(fs->bmap_disk, fs->bmap, bsz);
    for (uint64_t i = 0; i < fs->sb.ibitmap_blocks; i++) {
        rc = sfs_dev_read(&fs->dev, fs->sb.ibitmap_start + i,
                          fs->imap + i * SFS_BLOCK_SIZE);
        if (rc) return rc;
    }
    return sfs_runs_rebuild(fs);   /* derive free-run list from bitmap */
}

int sfs_mount(sfs_fs_t *fs, const char *path, int readonly)
{
    memset(fs, 0, sizeof(*fs));
    fs->readonly = readonly;
    int rc = sfs_dev_open(&fs->dev, path, 0, 0);
    if (rc) return rc;

    uint8_t buf[SFS_BLOCK_SIZE];
    rc = sfs_dev_read(&fs->dev, 0, buf);
    if (rc == 0)
        rc = sfs_super_decode(&fs->sb, buf);
    if (rc == 0 && fs->sb.total_blocks > fs->dev.nblocks)
        rc = -EINVAL;
    if (rc == 0)
        rc = sfs_journal_recover(fs);   /* replay/rollback before use */
    if (rc == 0)
        rc = sfs_reload_state(fs);      /* sb may have changed in replay */
    if (rc == 0) {
        fs->txn = malloc(sizeof(sfs_txn_entry_t) * SFS_TXN_MAX_BLOCKS);
        if (!fs->txn) rc = -ENOMEM;
    }
    if (rc == 0 && fs->sb.version == SFS_VERSION_V1) {
        if (fs->readonly)
            rc = -EROFS;   /* v1 needs migration, which must write */
        else
            rc = sfs_migrate_v1(fs);
    }
    if (rc) {
        sfs_unmount(fs);
        return rc;
    }
    fs->ialloc_hint = 1;
    return 0;
}

void sfs_unmount(sfs_fs_t *fs)
{
    if (fs->dev.fd >= 0)
        sfs_dev_close(&fs->dev);
    free(fs->bmap); free(fs->bmap_disk); free(fs->imap);
    free(fs->txn); free(fs->txn_freed); free(fs->runs);
    fs->bmap = fs->bmap_disk = fs->imap = NULL;
    fs->txn = NULL; fs->txn_freed = NULL; fs->runs = NULL;
}

/* ------------------------------------------------------- path handling */

/* split absolute path into normalized components ('.', '..' resolved) */
static int path_split(const char *path, char comps[][SFS_NAME_MAX + 1],
                      int max, int *ncomp)
{
    if (path[0] != '/')
        return -EINVAL;
    int n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len > SFS_NAME_MAX)
            return -ENAMETOOLONG;
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0) n--;
            continue;
        }
        if (n >= max)
            return -ENAMETOOLONG;
        memcpy(comps[n], start, len);
        comps[n][len] = '\0';
        n++;
    }
    *ncomp = n;
    return 0;
}

#define SFS_PATH_MAX_COMPS 64

static int resolve_comps(sfs_fs_t *fs, char comps[][SFS_NAME_MAX + 1],
                         int ncomp, uint32_t *ino_out)
{
    uint32_t ino = fs->sb.root_ino;
    for (int i = 0; i < ncomp; i++) {
        sfs_inode_t dir;
        int rc = sfs_iget(fs, ino, &dir);
        if (rc) return rc;
        if (dir.type != SFS_TYPE_DIR)
            return -ENOTDIR;
        rc = sfs_dir_lookup(fs, &dir, comps[i], &ino);
        if (rc) return rc;
    }
    *ino_out = ino;
    return 0;
}

int sfs_resolve(sfs_fs_t *fs, const char *path, uint32_t *ino_out)
{
    char comps[SFS_PATH_MAX_COMPS][SFS_NAME_MAX + 1];
    int n;
    int rc = path_split(path, comps, SFS_PATH_MAX_COMPS, &n);
    if (rc) return rc;
    return resolve_comps(fs, comps, n, ino_out);
}

int sfs_resolve_parent(sfs_fs_t *fs, const char *path, uint32_t *parent_out,
                       char *name_out)
{
    char comps[SFS_PATH_MAX_COMPS][SFS_NAME_MAX + 1];
    int n;
    int rc = path_split(path, comps, SFS_PATH_MAX_COMPS, &n);
    if (rc) return rc;
    if (n == 0)
        return -EINVAL;    /* root has no parent entry */
    rc = resolve_comps(fs, comps, n - 1, parent_out);
    if (rc) return rc;
    strcpy(name_out, comps[n - 1]);
    return 0;
}

/* --------------------------------------------------------- namespace ops */

static int create_node(sfs_fs_t *fs, const char *path, uint16_t type,
                       uint32_t *ino_out)
{
    uint32_t parent_ino;
    char name[SFS_NAME_MAX + 1];
    int rc = sfs_resolve_parent(fs, path, &parent_ino, name);
    if (rc) return rc;

    sfs_inode_t parent;
    rc = sfs_iget(fs, parent_ino, &parent);
    if (rc) return rc;
    if (parent.type != SFS_TYPE_DIR)
        return -ENOTDIR;

    uint32_t existing;
    if (sfs_dir_lookup(fs, &parent, name, &existing) == 0)
        return -EEXIST;

    uint32_t ino;
    rc = sfs_ialloc(fs, &ino);
    if (rc) return rc;

    sfs_inode_t node;
    memset(&node, 0, sizeof(node));
    node.type = type;
    node.nlinks = 1;
    node.flags = SFS_IFLAG_EXTENTS;
    node.ctime = node.mtime = node.atime = sfs_now(fs);
    if (type == SFS_TYPE_DIR) {
        rc = sfs_dir_init(fs, &node);   /* allocate bucket table */
        if (rc) return rc;
    }
    rc = sfs_iput(fs, ino, &node);
    if (rc) return rc;

    rc = sfs_dir_add(fs, parent_ino, &parent, name, ino);
    if (rc) return rc;
    rc = sfs_iput(fs, parent_ino, &parent);
    if (rc) return rc;
    if (ino_out) *ino_out = ino;
    return 0;
}

int sfs_mkdir(sfs_fs_t *fs, const char *path)
{
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;
    rc = create_node(fs, path, SFS_TYPE_DIR, NULL);
    if (rc) { sfs_txn_abort(fs); return rc; }
    return sfs_txn_commit(fs);
}

int sfs_creat(sfs_fs_t *fs, const char *path)
{
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;
    rc = create_node(fs, path, SFS_TYPE_FILE, NULL);
    if (rc) { sfs_txn_abort(fs); return rc; }
    return sfs_txn_commit(fs);
}

/* ----------------------------------------------------------- write path */

/* grow the file mapping to at least `need` blocks; tries to extend the
 * last extent in place first (contiguity), then best-fit extents */
static int grow_to(sfs_fs_t *fs, sfs_inode_t *node, uint32_t need)
{
    uint32_t have;
    int rc = sfs_inode_nblocks(fs, node, &have);
    if (rc) return rc;
    while (have < need) {
        uint32_t want = need - have;
        sfs_extent_t e;
        if (node->nextents > 0) {
            sfs_extent_t last;
            rc = sfs_ext_get(fs, node, node->nextents - 1, &last);
            if (rc) return rc;
            uint32_t got = sfs_ext_alloc_at(fs, last.start + last.count,
                                            want);
            if (got > 0) {
                e.start = last.start + last.count;
                e.count = got;
                rc = sfs_ext_append(fs, node, e);   /* coalesces */
                if (rc) return rc;
                have += got;
                continue;
            }
        }
        rc = sfs_ext_alloc(fs, want, &e);   /* may return fewer */
        if (rc) return rc;
        rc = sfs_ext_append(fs, node, e);
        if (rc) return rc;
        have += e.count;
    }
    return 0;
}

/* write [offset, offset+len) into already-mapped blocks; bytes below
 * old_size in partially-written blocks are preserved (read-modify-write) */
static int write_mapped(sfs_fs_t *fs, const sfs_inode_t *node,
                        uint64_t offset, const uint8_t *data, uint64_t len,
                        uint64_t old_size)
{
    uint64_t pos = offset;
    const uint8_t *p = data;
    uint8_t buf[SFS_BLOCK_SIZE];
    while (len > 0) {
        uint32_t lblk = (uint32_t)(pos / SFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % SFS_BLOCK_SIZE);
        uint32_t chunk = SFS_BLOCK_SIZE - boff;
        if (chunk > len) chunk = (uint32_t)len;
        uint64_t phys;
        int rc = sfs_inode_lookup_blk(fs, node, lblk, &phys);
        if (rc) return rc;
        if (phys == 0) return -EIO;
        if (chunk == SFS_BLOCK_SIZE) {
            memcpy(buf, p, SFS_BLOCK_SIZE);
        } else if ((uint64_t)lblk * SFS_BLOCK_SIZE < old_size) {
            rc = sfs_bread(fs, phys, buf);
            if (rc) return rc;
            memcpy(buf + boff, p, chunk);
        } else {
            memset(buf, 0, sizeof(buf));
            memcpy(buf + boff, p, chunk);
        }
        rc = sfs_bput(fs, phys, buf);
        if (rc) return rc;
        p += chunk; pos += chunk; len -= chunk;
    }
    return 0;
}

/* overwrite-in-place cap: blocks staged through the journal must leave
 * room for metadata in the same transaction */
#define SFS_INPLACE_MAX_BLOCKS 64u

int sfs_write_file(sfs_fs_t *fs, const char *path, const void *data,
                   uint64_t len, int append)
{
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;

    uint32_t ino;
    rc = sfs_resolve(fs, path, &ino);
    if (rc == -ENOENT)
        rc = create_node(fs, path, SFS_TYPE_FILE, &ino);
    if (rc) goto abort;

    sfs_inode_t node;
    rc = sfs_iget(fs, ino, &node);
    if (rc) goto abort;
    if (node.type != SFS_TYPE_FILE) { rc = -EISDIR; goto abort; }

    uint64_t offset = append ? node.size : 0;
    uint64_t old_size = node.size;
    uint32_t need = (uint32_t)((offset + len + SFS_BLOCK_SIZE - 1)
                               / SFS_BLOCK_SIZE);

    if (!append && node.size > 0) {
        uint32_t have;
        rc = sfs_inode_nblocks(fs, &node, &have);
        if (rc) goto abort;
        if (len > 0 && need <= have && need <= SFS_INPLACE_MAX_BLOCKS) {
            /* overwrite in place: keep the mapping (minus the tail),
             * journal the data blocks */
            rc = sfs_inode_shrink(fs, &node, need);
        } else {
            /* full rewrite: release old extents (re-enter the free list
             * only at commit), allocate fresh ones */
            rc = sfs_inode_truncate(fs, &node);
        }
        if (rc) goto abort;
        old_size = 0;   /* no live bytes to preserve */
    }

    rc = grow_to(fs, &node, need);
    if (rc) goto abort;
    rc = write_mapped(fs, &node, offset, data, len, old_size);
    if (rc) goto abort;

    if (offset + len > node.size || !append)
        node.size = offset + len;
    node.mtime = sfs_now(fs);
    rc = sfs_iput(fs, ino, &node);
    if (rc) goto abort;
    return sfs_txn_commit(fs);

abort:
    sfs_txn_abort(fs);
    return rc;
}

int sfs_read_file(sfs_fs_t *fs, const char *path, uint8_t **out,
                  uint64_t *len)
{
    uint32_t ino;
    int rc = sfs_resolve(fs, path, &ino);
    if (rc) return rc;
    sfs_inode_t node;
    rc = sfs_iget(fs, ino, &node);
    if (rc) return rc;
    if (node.type != SFS_TYPE_FILE)
        return -EISDIR;

    uint8_t *buf = malloc(node.size ? node.size : 1);
    if (!buf) return -ENOMEM;

    uint64_t pos = 0;
    uint8_t blk[SFS_BLOCK_SIZE];
    while (pos < node.size) {
        uint32_t lblk = (uint32_t)(pos / SFS_BLOCK_SIZE);
        uint64_t phys;
        rc = sfs_inode_lookup_blk(fs, &node, lblk, &phys);
        if (rc == 0 && phys == 0)
            rc = -EIO;   /* v2 files have no holes */
        if (rc) { free(buf); return rc; }
        uint64_t chunk = node.size - pos;
        if (chunk > SFS_BLOCK_SIZE) chunk = SFS_BLOCK_SIZE;
        rc = sfs_bread(fs, phys, blk);
        if (rc) { free(buf); return rc; }
        memcpy(buf + pos, blk, chunk);
        pos += chunk;
    }

    /* access-time update (best effort, skipped on read-only mounts) */
    if (!fs->readonly) {
        if (sfs_txn_begin(fs) == 0) {
            node.atime = sfs_now(fs);
            if (sfs_iput(fs, ino, &node) == 0)
                sfs_txn_commit(fs);
            else
                sfs_txn_abort(fs);
        }
    }

    *out = buf;
    *len = node.size;
    return 0;
}

int sfs_unlink(sfs_fs_t *fs, const char *path)
{
    int rc = sfs_txn_begin(fs);
    if (rc) return rc;

    uint32_t parent_ino, ino;
    char name[SFS_NAME_MAX + 1];
    rc = sfs_resolve_parent(fs, path, &parent_ino, name);
    if (rc) goto abort;

    sfs_inode_t parent;
    rc = sfs_iget(fs, parent_ino, &parent);
    if (rc) goto abort;
    rc = sfs_dir_lookup(fs, &parent, name, &ino);
    if (rc) goto abort;

    sfs_inode_t node;
    rc = sfs_iget(fs, ino, &node);
    if (rc) goto abort;

    if (node.type == SFS_TYPE_DIR) {
        rc = sfs_dir_is_empty(fs, &node);
        if (rc < 0) goto abort;
        if (rc == 0) { rc = -ENOTEMPTY; goto abort; }
    }

    rc = sfs_dir_remove(fs, parent_ino, &parent, name);
    if (rc) goto abort;
    rc = sfs_iput(fs, parent_ino, &parent);
    if (rc) goto abort;

    if (node.nlinks <= 1) {
        rc = sfs_inode_truncate(fs, &node);
        if (rc) goto abort;
        memset(&node, 0, sizeof(node));   /* type FREE */
        rc = sfs_iput(fs, ino, &node);
        if (rc) goto abort;
        rc = sfs_ifree(fs, ino);
        if (rc) goto abort;
    } else {
        node.nlinks--;
        rc = sfs_iput(fs, ino, &node);
        if (rc) goto abort;
    }
    return sfs_txn_commit(fs);

abort:
    sfs_txn_abort(fs);
    return rc;
}

int sfs_rename(sfs_fs_t *fs, const char *from, const char *to)
{
    char fc[SFS_PATH_MAX_COMPS][SFS_NAME_MAX + 1];
    char tc[SFS_PATH_MAX_COMPS][SFS_NAME_MAX + 1];
    int nf, nt;
    int rc = path_split(from, fc, SFS_PATH_MAX_COMPS, &nf);
    if (rc) return rc;
    rc = path_split(to, tc, SFS_PATH_MAX_COMPS, &nt);
    if (rc) return rc;
    if (nf == 0)
        return -EINVAL;   /* cannot move the root */
    if (nt == 0)
        return -EEXIST;

    /* lexical subtree check on normalized components */
    if (nf <= nt) {
        int prefix = 1;
        for (int i = 0; i < nf && prefix; i++)
            if (strcmp(fc[i], tc[i]) != 0)
                prefix = 0;
        if (prefix && nf == nt)
            return 0;        /* same path: no-op */
        if (prefix)
            return -EINVAL;  /* moving a dir into its own subtree */
    }

    rc = sfs_txn_begin(fs);
    if (rc) return rc;

    uint32_t fparent, tparent, ino, existing;
    rc = resolve_comps(fs, fc, nf - 1, &fparent);
    if (rc) goto abort;
    rc = resolve_comps(fs, tc, nt - 1, &tparent);
    if (rc) goto abort;

    sfs_inode_t fdir;
    rc = sfs_iget(fs, fparent, &fdir);
    if (rc) goto abort;
    rc = sfs_dir_lookup(fs, &fdir, fc[nf - 1], &ino);
    if (rc) goto abort;

    if (fparent == tparent) {
        if (sfs_dir_lookup(fs, &fdir, tc[nt - 1], &existing) == 0) {
            rc = -EEXIST; goto abort;
        }
        rc = sfs_dir_remove(fs, fparent, &fdir, fc[nf - 1]);
        if (rc) goto abort;
        rc = sfs_dir_add(fs, fparent, &fdir, tc[nt - 1], ino);
        if (rc) goto abort;
        rc = sfs_iput(fs, fparent, &fdir);
        if (rc) goto abort;
    } else {
        sfs_inode_t tdir;
        rc = sfs_iget(fs, tparent, &tdir);
        if (rc) goto abort;
        if (tdir.type != SFS_TYPE_DIR) { rc = -ENOTDIR; goto abort; }
        if (sfs_dir_lookup(fs, &tdir, tc[nt - 1], &existing) == 0) {
            rc = -EEXIST; goto abort;
        }
        rc = sfs_dir_add(fs, tparent, &tdir, tc[nt - 1], ino);
        if (rc) goto abort;
        rc = sfs_dir_remove(fs, fparent, &fdir, fc[nf - 1]);
        if (rc) goto abort;
        rc = sfs_iput(fs, tparent, &tdir);
        if (rc) goto abort;
        rc = sfs_iput(fs, fparent, &fdir);
        if (rc) goto abort;
    }
    return sfs_txn_commit(fs);

abort:
    sfs_txn_abort(fs);
    return rc;
}

int sfs_stat_path(sfs_fs_t *fs, const char *path, uint32_t *ino_out,
                  sfs_inode_t *out)
{
    uint32_t ino;
    int rc = sfs_resolve(fs, path, &ino);
    if (rc) return rc;
    rc = sfs_iget(fs, ino, out);
    if (rc) return rc;
    if (ino_out) *ino_out = ino;
    return 0;
}
