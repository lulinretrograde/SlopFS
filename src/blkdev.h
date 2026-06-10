/* Block device layer: positioned 4K-block I/O against a disk image file. */
#ifndef SLOPFS_BLKDEV_H
#define SLOPFS_BLKDEV_H

#include <stdint.h>

typedef struct {
    int fd;
    uint64_t nblocks;
} sfs_dev_t;

int  sfs_dev_open(sfs_dev_t *dev, const char *path, int create, uint64_t nblocks);
void sfs_dev_close(sfs_dev_t *dev);
int  sfs_dev_read(sfs_dev_t *dev, uint64_t blk, void *buf);
int  sfs_dev_write(sfs_dev_t *dev, uint64_t blk, const void *buf);
int  sfs_dev_sync(sfs_dev_t *dev);

#endif
