/* slopfs-fsck: offline integrity checker.
 *
 *   slopfs-fsck [--deep] [--repair] [--quiet] disk.img
 *
 * Exit codes: 0 = clean, 1 = problems found, 2 = usage/I-O error.
 * Mounting runs journal recovery first, so a crashed-but-committed
 * transaction is checkpointed before checking (as on any mount).
 */
#define _GNU_SOURCE
#include "fs.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int deep = 0, repair = 0, verbose = 1;
    const char *img = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--deep")) deep = 1;
        else if (!strcmp(argv[i], "--repair")) repair = 1;
        else if (!strcmp(argv[i], "--quiet")) verbose = 0;
        else if (argv[i][0] == '-') img = NULL;
        else { img = argv[i]; continue; }
    }
    if (!img) {
        fprintf(stderr,
                "usage: slopfs-fsck [--deep] [--repair] [--quiet] "
                "disk.img\n");
        return 2;
    }

    sfs_fs_t fs;
    int rc = sfs_mount(&fs, img, 0);
    if (rc) {
        fprintf(stderr, "slopfs-fsck: mount failed: %s\n", strerror(-rc));
        return 2;
    }

    int problems = sfs_check(&fs, deep, repair, verbose);
    sfs_unmount(&fs);
    if (problems < 0) {
        fprintf(stderr, "slopfs-fsck: check failed: %s\n",
                strerror(-problems));
        return 2;
    }
    if (problems == 0) {
        printf("%s: clean%s\n", img, deep ? " (deep)" : "");
        return 0;
    }
    printf("%s: %d problem%s%s\n", img, problems, problems == 1 ? "" : "s",
           repair ? " remaining after repair" : " found");
    return 1;
}
