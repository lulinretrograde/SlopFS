#define _GNU_SOURCE
#include "codec.h"
#include "format.h"
#include <string.h>

uint32_t sfs_crc32(uint32_t seed, const void *data, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = 1;
    }
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    const uint8_t *p = data;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- superblock ----
 * off  size  field
 * 0    8     magic
 * 8    4     version
 * 12   4     block_size
 * 16   8     total_blocks
 * 24   4     inode_count
 * 28   4     root_ino
 * 32   8     bitmap_start
 * 40   8     bitmap_blocks
 * 48   8     ibitmap_start
 * 56   8     ibitmap_blocks
 * 64   8     inode_start
 * 72   8     inode_blocks
 * 80   8     journal_start
 * 88   8     journal_blocks
 * 96   8     data_start
 * 104  8     free_blocks
 * 112  8     free_inodes
 * 120  8     generation
 * 128  4     crc32 of bytes [0,128)
 */
void sfs_super_encode(const sfs_super_t *sb, uint8_t *buf)
{
    memset(buf, 0, SFS_BLOCK_SIZE);
    memcpy(buf, SFS_MAGIC, SFS_MAGIC_LEN);
    put_le32(buf + 8, sb->version);
    put_le32(buf + 12, sb->block_size);
    put_le64(buf + 16, sb->total_blocks);
    put_le32(buf + 24, sb->inode_count);
    put_le32(buf + 28, sb->root_ino);
    put_le64(buf + 32, sb->bitmap_start);
    put_le64(buf + 40, sb->bitmap_blocks);
    put_le64(buf + 48, sb->ibitmap_start);
    put_le64(buf + 56, sb->ibitmap_blocks);
    put_le64(buf + 64, sb->inode_start);
    put_le64(buf + 72, sb->inode_blocks);
    put_le64(buf + 80, sb->journal_start);
    put_le64(buf + 88, sb->journal_blocks);
    put_le64(buf + 96, sb->data_start);
    put_le64(buf + 104, sb->free_blocks);
    put_le64(buf + 112, sb->free_inodes);
    put_le64(buf + 120, sb->generation);
    put_le32(buf + 128, sfs_crc32(0, buf, 128));
}

int sfs_super_decode(sfs_super_t *sb, const uint8_t *buf)
{
    if (memcmp(buf, SFS_MAGIC, SFS_MAGIC_LEN) != 0)
        return -1;
    if (get_le32(buf + 128) != sfs_crc32(0, buf, 128))
        return -1;
    sb->version       = get_le32(buf + 8);
    sb->block_size    = get_le32(buf + 12);
    sb->total_blocks  = get_le64(buf + 16);
    sb->inode_count   = get_le32(buf + 24);
    sb->root_ino      = get_le32(buf + 28);
    sb->bitmap_start  = get_le64(buf + 32);
    sb->bitmap_blocks = get_le64(buf + 40);
    sb->ibitmap_start = get_le64(buf + 48);
    sb->ibitmap_blocks= get_le64(buf + 56);
    sb->inode_start   = get_le64(buf + 64);
    sb->inode_blocks  = get_le64(buf + 72);
    sb->journal_start = get_le64(buf + 80);
    sb->journal_blocks= get_le64(buf + 88);
    sb->data_start    = get_le64(buf + 96);
    sb->free_blocks   = get_le64(buf + 104);
    sb->free_inodes   = get_le64(buf + 112);
    sb->generation    = get_le64(buf + 120);
    if ((sb->version != SFS_VERSION_V1 && sb->version != SFS_VERSION_V2) ||
        sb->block_size != SFS_BLOCK_SIZE)
        return -1;
    return 0;
}

/* ---- inode v2 (128 bytes) ----
 * 0   2  type
 * 2   2  nlinks
 * 4   4  flags (bit 0: extent-based)
 * 8   8  size
 * 16  8  ctime
 * 24  8  mtime
 * 32  8  atime
 * 40  4  nextents (total, inline + overflow)
 * 44  4  ext_block (overflow extent block, 0 = none)
 * 48  80 ext[10]: (start u32, count u32) pairs
 */
void sfs_inode_encode(const sfs_inode_t *in, uint8_t *buf)
{
    memset(buf, 0, SFS_INODE_SIZE);
    put_le16(buf + 0, in->type);
    put_le16(buf + 2, in->nlinks);
    put_le32(buf + 4, in->flags);
    put_le64(buf + 8, in->size);
    put_le64(buf + 16, in->ctime);
    put_le64(buf + 24, in->mtime);
    put_le64(buf + 32, in->atime);
    put_le32(buf + 40, in->nextents);
    put_le32(buf + 44, in->ext_block);
    for (unsigned i = 0; i < SFS_INLINE_EXTENTS; i++) {
        put_le32(buf + 48 + i * 8, in->ext[i].start);
        put_le32(buf + 52 + i * 8, in->ext[i].count);
    }
}

static void inode_decode_common(sfs_inode_t *in, const uint8_t *buf)
{
    memset(in, 0, sizeof(*in));
    in->type   = get_le16(buf + 0);
    in->nlinks = get_le16(buf + 2);
    in->flags  = get_le32(buf + 4);
    in->size   = get_le64(buf + 8);
    in->ctime  = get_le64(buf + 16);
    in->mtime  = get_le64(buf + 24);
    in->atime  = get_le64(buf + 32);
}

void sfs_inode_decode(sfs_inode_t *in, const uint8_t *buf)
{
    inode_decode_common(in, buf);
    in->nextents = get_le32(buf + 40);
    in->ext_block = get_le32(buf + 44);
    for (unsigned i = 0; i < SFS_INLINE_EXTENTS; i++) {
        in->ext[i].start = get_le32(buf + 48 + i * 8);
        in->ext[i].count = get_le32(buf + 52 + i * 8);
    }
}

/* v1 layout: 40..88 direct[12], 88 indirect (migration source only) */
void sfs_inode_decode_v1(sfs_inode_t *in, const uint8_t *buf)
{
    inode_decode_common(in, buf);
    for (unsigned i = 0; i < SFS_NDIRECT_V1; i++)
        in->v1_direct[i] = get_le32(buf + 40 + i * 4);
    in->v1_indirect = get_le32(buf + 88);
}

/* ---- dirent v2 (512 bytes) ----
 * 0   4    ino (0 = empty)
 * 4   2    name_len
 * 8   256  name (NUL-padded)
 * 264 4    next slot in hash chain (SFS_DIR_EOC = end)
 */
void sfs_dirent_encode(const sfs_dirent_t *de, uint8_t *buf)
{
    memset(buf, 0, SFS_DIRENT_SIZE);
    size_t len = strnlen(de->name, SFS_NAME_MAX);
    put_le32(buf + 0, de->ino);
    put_le16(buf + 4, (uint16_t)len);
    memcpy(buf + 8, de->name, len);
    put_le32(buf + 264, de->next);
}

void sfs_dirent_decode(sfs_dirent_t *de, const uint8_t *buf)
{
    de->ino = get_le32(buf + 0);
    uint16_t len = get_le16(buf + 4);
    if (len > SFS_NAME_MAX) len = SFS_NAME_MAX;
    memcpy(de->name, buf + 8, len);
    de->name[len] = '\0';
    de->next = get_le32(buf + 264);
}

uint32_t sfs_name_hash(const char *name)
{
    uint32_t h = 2166136261u;          /* FNV-1a */
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}
