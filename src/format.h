/*
 * SlopFS on-disk format definitions.
 *
 * Disk layout (block 0 first):
 *   [ SUPERBLOCK ] [ BLOCK BITMAP ] [ INODE BITMAP ] [ INODE TABLE ]
 *   [ JOURNAL ] [ DATA BLOCKS ... ]
 *
 * Two format versions exist:
 *   v1: block-pointer inodes (12 direct + 1 indirect), linear dirents.
 *       Read-only; supported only as a migration source.
 *   v2: extent-based inodes (10 inline extents + 1 extent overflow
 *       block), hash-chained directory index. Current format.
 *
 * All on-disk integers are little-endian, written via explicit
 * serialization (see codec.h). Structs below are in-memory
 * representations only and are never dumped raw to disk.
 */
#ifndef SLOPFS_FORMAT_H
#define SLOPFS_FORMAT_H

#include <stdint.h>

#define SFS_BLOCK_SIZE      4096u
#define SFS_MAGIC           "SLOPFS\0\0"   /* 8 bytes incl. padding */
#define SFS_MAGIC_LEN       8
#define SFS_VERSION_V1      1u
#define SFS_VERSION_V2      2u

#define SFS_INODE_SIZE      128u
#define SFS_INODES_PER_BLK  (SFS_BLOCK_SIZE / SFS_INODE_SIZE)   /* 32 */

/* v2 extents */
#define SFS_INLINE_EXTENTS  10u
#define SFS_EXT_PER_BLK     (SFS_BLOCK_SIZE / 8u)               /* 512 */
#define SFS_MAX_EXTENTS     (SFS_INLINE_EXTENTS + SFS_EXT_PER_BLK)
#define SFS_IFLAG_EXTENTS   0x1u   /* inode uses the v2 extent format */

/* v1 (migration source only) */
#define SFS_NDIRECT_V1      12u
#define SFS_PTRS_PER_BLK_V1 (SFS_BLOCK_SIZE / 4u)

#define SFS_NAME_MAX        255u
#define SFS_DIRENT_SIZE     512u
#define SFS_DIRENTS_PER_BLK (SFS_BLOCK_SIZE / SFS_DIRENT_SIZE)  /* 8 */

/* v2 directory hash index: logical block 0 of every directory is the
 * bucket table; entry slots start at logical block 1.
 * slot s lives at logical block 1 + s/8, slot index s%8. */
#define SFS_DIR_NBUCKETS    1023u
#define SFS_DIR_HINT_OFF    4092u  /* free-slot search hint, last 4 bytes */
#define SFS_DIR_EOC         0xFFFFFFFFu   /* empty bucket / end of chain */

#define SFS_ROOT_INO        1u   /* inode 0 is reserved/invalid */

/* inode types */
#define SFS_TYPE_FREE       0u
#define SFS_TYPE_FILE       1u
#define SFS_TYPE_DIR        2u

/* journal */
#define SFS_JMAGIC          "SLOPJRNL"
#define SFS_JMAGIC_LEN      8
#define SFS_JSTATE_CLEAN    0u
#define SFS_JSTATE_COMMITTED 1u
/* max block images per transaction: limited by target list in header */
#define SFS_TXN_MAX_BLOCKS  256u

typedef struct {
    uint32_t start;   /* first block, 0 = unused extent slot */
    uint32_t count;   /* contiguous block count */
} sfs_extent_t;

/* In-memory superblock */
typedef struct {
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint32_t inode_count;
    uint32_t root_ino;
    uint64_t bitmap_start, bitmap_blocks;     /* block allocation bitmap */
    uint64_t ibitmap_start, ibitmap_blocks;   /* inode allocation bitmap */
    uint64_t inode_start, inode_blocks;       /* inode table */
    uint64_t journal_start, journal_blocks;
    uint64_t data_start;
    uint64_t free_blocks;
    uint64_t free_inodes;
    uint64_t generation;   /* bumped on every committed txn (for live view) */
} sfs_super_t;

/* In-memory inode. v2 fields are authoritative; the v1 fields are only
 * populated by sfs_inode_decode_v1 during migration. */
typedef struct {
    uint16_t type;
    uint16_t nlinks;
    uint32_t flags;
    uint64_t size;
    uint64_t ctime, mtime, atime;
    /* v2 */
    uint32_t nextents;
    uint32_t ext_block;                      /* overflow extent block */
    sfs_extent_t ext[SFS_INLINE_EXTENTS];
    /* v1 migration source */
    uint32_t v1_direct[SFS_NDIRECT_V1];
    uint32_t v1_indirect;
} sfs_inode_t;

/* In-memory directory entry */
typedef struct {
    uint32_t ino;                    /* 0 = empty slot */
    uint32_t next;                   /* next slot in hash chain, or EOC */
    char     name[SFS_NAME_MAX + 1]; /* NUL-terminated */
} sfs_dirent_t;

/* Serialization */
void sfs_super_encode(const sfs_super_t *sb, uint8_t *buf);
int  sfs_super_decode(sfs_super_t *sb, const uint8_t *buf);
void sfs_inode_encode(const sfs_inode_t *in, uint8_t *buf);
void sfs_inode_decode(sfs_inode_t *in, const uint8_t *buf);
void sfs_inode_decode_v1(sfs_inode_t *in, const uint8_t *buf);
void sfs_dirent_encode(const sfs_dirent_t *de, uint8_t *buf);
void sfs_dirent_decode(sfs_dirent_t *de, const uint8_t *buf);
uint32_t sfs_name_hash(const char *name);

#endif
