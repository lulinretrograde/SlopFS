#define _GNU_SOURCE
#include "blkdev.h"
#include "format.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

int sfs_dev_open(sfs_dev_t *dev, const char *path, int create, uint64_t nblocks)
{
    int flags = O_RDWR | (create ? O_CREAT | O_TRUNC : 0);
    dev->fd = open(path, flags, 0644);
    if (dev->fd < 0)
        return -errno;

    if (create) {
        if (ftruncate(dev->fd, (off_t)(nblocks * SFS_BLOCK_SIZE)) != 0) {
            int e = -errno;
            close(dev->fd);
            dev->fd = -1;
            return e;
        }
        dev->nblocks = nblocks;
    } else {
        struct stat st;
        if (fstat(dev->fd, &st) != 0) {
            int e = -errno;
            close(dev->fd);
            dev->fd = -1;
            return e;
        }
        dev->nblocks = (uint64_t)st.st_size / SFS_BLOCK_SIZE;
    }
    return 0;
}

void sfs_dev_close(sfs_dev_t *dev)
{
    if (dev->fd >= 0) {
        fsync(dev->fd);
        close(dev->fd);
        dev->fd = -1;
    }
}

int sfs_dev_read(sfs_dev_t *dev, uint64_t blk, void *buf)
{
    if (blk >= dev->nblocks)
        return -EINVAL;
    uint8_t *p = buf;
    size_t left = SFS_BLOCK_SIZE;
    off_t off = (off_t)(blk * SFS_BLOCK_SIZE);
    while (left > 0) {
        ssize_t n = pread(dev->fd, p, left, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        p += n; off += n; left -= (size_t)n;
    }
    return 0;
}

int sfs_dev_write(sfs_dev_t *dev, uint64_t blk, const void *buf)
{
    if (blk >= dev->nblocks)
        return -EINVAL;
    const uint8_t *p = buf;
    size_t left = SFS_BLOCK_SIZE;
    off_t off = (off_t)(blk * SFS_BLOCK_SIZE);
    while (left > 0) {
        ssize_t n = pwrite(dev->fd, p, left, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        p += n; off += n; left -= (size_t)n;
    }
    return 0;
}

int sfs_dev_sync(sfs_dev_t *dev)
{
    return fsync(dev->fd) == 0 ? 0 : -errno;
}
