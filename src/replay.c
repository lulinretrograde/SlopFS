/*
 * Deterministic replay: `slopfs replay disk.img <seed> [nops]`.
 *
 * Creates a fresh image and applies a pseudo-random workload derived
 * entirely from the seed (xorshift64). Determinism sources:
 *   - SLOPFS_FAKE_TIME makes mkfs timestamps fixed
 *   - fs->fake_now turns every sfs_now() into a counter
 *   - the allocator is best-fit with lowest-start tie-break (no
 *     randomness, no wall-clock dependence)
 * Two runs with the same seed therefore produce byte-identical images.
 */
#define _GNU_SOURCE
#include "fs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_BLOCKS   16384u   /* 64 MB image */
#define MAX_LIVE        512u

static uint64_t xs64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void fill(uint8_t *buf, size_t n, uint64_t *rng)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = xs64(rng);
        size_t c = n - i < 8 ? n - i : 8;
        memcpy(buf + i, &v, c);
    }
}

int sfs_replay(const char *path, uint64_t seed, uint32_t nops)
{
    setenv("SLOPFS_FAKE_TIME", "1", 1);
    int rc = sfs_mkfs(path, REPLAY_BLOCKS);
    if (rc) return rc;

    sfs_fs_t fs;
    rc = sfs_mount(&fs, path, 0);
    if (rc) return rc;
    fs.fake_now = 1000;   /* deterministic clock */

    uint64_t rng = seed ? seed : 0x5105f5b00b5ull;
    uint32_t live[MAX_LIVE];   /* ids of existing files */
    uint32_t nlive = 0, next_id = 0;
    uint8_t *data = malloc(64 * SFS_BLOCK_SIZE);
    if (!data) { sfs_unmount(&fs); return -ENOMEM; }

    for (unsigned d = 0; d < 8; d++) {
        char p[32];
        snprintf(p, sizeof(p), "/r%u", d);
        rc = sfs_mkdir(&fs, p);
        if (rc) goto out;
    }

    uint32_t applied = 0;
    for (uint32_t op = 0; op < nops; op++) {
        uint32_t r = (uint32_t)(xs64(&rng) % 100);
        char p[64], q[64];
        if (r < 35 || nlive == 0) {                    /* create + write */
            if (nlive >= MAX_LIVE) continue;
            uint32_t id = next_id++;
            snprintf(p, sizeof(p), "/r%u/f%u",
                     (unsigned)(xs64(&rng) % 8), id);
            size_t len = (size_t)(xs64(&rng) % (48 * SFS_BLOCK_SIZE)) + 1;
            fill(data, len, &rng);
            rc = sfs_write_file(&fs, p, data, len, 0);
            if (rc == 0) live[nlive++] = id;
            else if (rc != -ENOSPC) goto out;
        } else {
            uint32_t pick = (uint32_t)(xs64(&rng) % nlive);
            uint32_t id = live[pick];
            /* the dir of a file is derived from earlier rolls; find it */
            int found = -1;
            for (unsigned d = 0; d < 8 && found < 0; d++) {
                uint32_t ino;
                snprintf(p, sizeof(p), "/r%u/f%u", d, id);
                if (sfs_resolve(&fs, p, &ino) == 0) found = (int)d;
            }
            if (found < 0) { live[pick] = live[--nlive]; continue; }
            snprintf(p, sizeof(p), "/r%u/f%u", (unsigned)found, id);

            if (r < 60) {                              /* append */
                size_t len = (size_t)(xs64(&rng) % (16 * SFS_BLOCK_SIZE))
                             + 1;
                fill(data, len, &rng);
                rc = sfs_write_file(&fs, p, data, len, 1);
                if (rc && rc != -ENOSPC && rc != -EFBIG) goto out;
            } else if (r < 75) {                       /* overwrite */
                size_t len = (size_t)(xs64(&rng) % (32 * SFS_BLOCK_SIZE))
                             + 1;
                fill(data, len, &rng);
                rc = sfs_write_file(&fs, p, data, len, 0);
                if (rc && rc != -ENOSPC) goto out;
            } else if (r < 90) {                       /* delete */
                rc = sfs_unlink(&fs, p);
                if (rc) goto out;
                live[pick] = live[--nlive];
            } else {                                   /* rename */
                snprintf(q, sizeof(q), "/r%u/f%u",
                         (unsigned)(xs64(&rng) % 8), id);
                rc = sfs_rename(&fs, p, q);
                if (rc && rc != -EEXIST) goto out;
            }
        }
        applied++;
    }
    rc = 0;
    printf("replay: seed=%llu ops=%u applied=%u files=%u generation=%llu\n",
           (unsigned long long)seed, nops, applied, nlive,
           (unsigned long long)fs.sb.generation);

out:
    free(data);
    sfs_unmount(&fs);
    return rc;
}
