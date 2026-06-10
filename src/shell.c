/* slopfs CLI: image creation + interactive shell. */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

static const char *errstr(int rc) { return strerror(-rc); }

/* parse sizes like 4096, 64K, 100MB, 1GB */
static int parse_size(const char *s, uint64_t *out)
{
    char *end;
    uint64_t v = strtoull(s, &end, 10);
    if (end == s) return -1;
    uint64_t mult = 1;
    if (*end) {
        switch (toupper((unsigned char)*end)) {
        case 'K': mult = 1024ull; break;
        case 'M': mult = 1024ull * 1024; break;
        case 'G': mult = 1024ull * 1024 * 1024; break;
        default: return -1;
        }
        end++;
        if (*end && toupper((unsigned char)*end) == 'B') end++;
        if (*end) return -1;
    }
    *out = v * mult;
    return 0;
}

/* tokenize a command line; supports "double quoted" arguments */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (argc >= max) return -1;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

static const char *type_str(uint16_t t)
{
    return t == SFS_TYPE_DIR ? "dir" : t == SFS_TYPE_FILE ? "file" : "free";
}

static void fmt_time(uint64_t t, char *buf, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

struct ls_arg { sfs_fs_t *fs; };
static int ls_cb(const sfs_dirent_t *de, void *p)
{
    struct ls_arg *a = p;
    sfs_inode_t in;
    if (sfs_iget(a->fs, de->ino, &in) == 0)
        printf("%-6s %10llu  ino=%-6u %s%s\n", type_str(in.type),
               (unsigned long long)in.size, de->ino, de->name,
               in.type == SFS_TYPE_DIR ? "/" : "");
    else
        printf("?      %s\n", de->name);
    return 0;
}

static void cmd_stat(sfs_fs_t *fs, const char *path)
{
    uint32_t ino;
    sfs_inode_t in;
    int rc = sfs_stat_path(fs, path, &ino, &in);
    if (rc) { fprintf(stderr, "stat: %s: %s\n", path, errstr(rc)); return; }

    char ct[32], mt[32], at[32];
    fmt_time(in.ctime, ct, sizeof(ct));
    fmt_time(in.mtime, mt, sizeof(mt));
    fmt_time(in.atime, at, sizeof(at));

    uint32_t nblk = 0;
    sfs_inode_nblocks(fs, &in, &nblk);

    printf("  path:     %s\n", path);
    printf("  inode:    %u\n", ino);
    printf("  type:     %s\n", type_str(in.type));
    printf("  size:     %llu bytes\n", (unsigned long long)in.size);
    printf("  links:    %u\n", in.nlinks);
    printf("  extents:  %u (%u blocks%s)\n", in.nextents, nblk,
           in.ext_block ? ", overflow block in use" : "");
    printf("  frag:     %.2f\n",
           in.nextents > 1 && nblk > 1
               ? (double)(in.nextents - 1) / (double)(nblk - 1) : 0.0);
    printf("  created:  %s\n", ct);
    printf("  modified: %s\n", mt);
    printf("  accessed: %s\n", at);
    if (in.nextents) {
        printf("  extent list:");
        for (uint32_t i = 0; i < in.nextents && i < 8; i++) {
            sfs_extent_t e;
            if (sfs_ext_get(fs, &in, i, &e))
                break;
            printf(" [%u+%u]", e.start, e.count);
        }
        if (in.nextents > 8) printf(" ... (+%u more)", in.nextents - 8);
        printf("\n");
    }
}

static void cmd_stats(sfs_fs_t *fs)
{
    sfs_stats_t st;
    int rc = sfs_collect_stats(fs, &st);
    if (rc) { fprintf(stderr, "stats: %s\n", errstr(rc)); return; }
    uint64_t used_inodes = st.inode_count - 1 - st.free_inodes;
    printf("  free space:        %llu blocks (%llu MB) of %llu data "
           "blocks\n",
           (unsigned long long)st.free_blocks,
           (unsigned long long)(st.free_blocks * SFS_BLOCK_SIZE >> 20),
           (unsigned long long)st.data_blocks);
    printf("  fragmentation:     %.4f (1 - largest_run/free)\n",
           st.frag_ratio);
    printf("  largest free run:  %u blocks (%u MB)\n",
           st.largest_free_run,
           st.largest_free_run * SFS_BLOCK_SIZE >> 20);
    printf("  files/dirs:        %u files, %u dirs\n", st.nfiles, st.ndirs);
    printf("  avg extents/file:  %.2f\n", st.avg_extents_per_file);
    printf("  inode utilization: %llu of %u (%.1f%%)\n",
           (unsigned long long)used_inodes, st.inode_count,
           100.0 * (double)used_inodes / (double)st.inode_count);
    printf("  journal:           %llu blocks, last commit used %u "
           "(%.1f%%)\n",
           (unsigned long long)st.journal_blocks, st.journal_last_txn,
           100.0 * (double)st.journal_last_txn
               / (double)st.journal_blocks);
}

static void cmd_df(sfs_fs_t *fs)
{
    const sfs_super_t *sb = &fs->sb;
    printf("  magic:        SLOPFS v%u\n", sb->version);
    printf("  block size:   %u\n", sb->block_size);
    printf("  total blocks: %llu (%llu MB)\n",
           (unsigned long long)sb->total_blocks,
           (unsigned long long)(sb->total_blocks * SFS_BLOCK_SIZE >> 20));
    printf("  free blocks:  %llu\n", (unsigned long long)sb->free_blocks);
    printf("  inodes:       %u total, %llu free\n", sb->inode_count,
           (unsigned long long)sb->free_inodes);
    printf("  layout:       bitmap@%llu ibitmap@%llu inodes@%llu "
           "journal@%llu data@%llu\n",
           (unsigned long long)sb->bitmap_start,
           (unsigned long long)sb->ibitmap_start,
           (unsigned long long)sb->inode_start,
           (unsigned long long)sb->journal_start,
           (unsigned long long)sb->data_start);
    printf("  generation:   %llu\n", (unsigned long long)sb->generation);
    printf("  largest free run: %u blocks\n", sfs_largest_free_run(fs));
}

static int do_write(sfs_fs_t *fs, const char *path, const char *text,
                    int append)
{
    int rc = sfs_write_file(fs, path, text, strlen(text), append);
    if (rc)
        fprintf(stderr, "write: %s: %s\n", path, errstr(rc));
    return rc;
}

static int cmd_import(sfs_fs_t *fs, const char *host, const char *dst)
{
    FILE *f = fopen(host, "rb");
    if (!f) { fprintf(stderr, "import: %s: %s\n", host, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf || (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len)) {
        fprintf(stderr, "import: read failed\n");
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    int rc = sfs_write_file(fs, dst, buf, (uint64_t)len, 0);
    free(buf);
    if (rc) fprintf(stderr, "import: %s: %s\n", dst, errstr(rc));
    return rc;
}

static int cmd_export(sfs_fs_t *fs, const char *src, const char *host)
{
    uint8_t *buf; uint64_t len;
    int rc = sfs_read_file(fs, src, &buf, &len);
    if (rc) { fprintf(stderr, "export: %s: %s\n", src, errstr(rc)); return rc; }
    FILE *f = fopen(host, "wb");
    if (!f) { fprintf(stderr, "export: %s: %s\n", host, strerror(errno)); free(buf); return -1; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return 0;
}

static void help(void)
{
    puts("commands:\n"
         "  mkdir <path>              create directory\n"
         "  touch <path>              create empty file\n"
         "  write <path> \"text\"       replace file content\n"
         "  append <path> \"text\"      append to file\n"
         "  read|cat <path>           print file content\n"
         "  ls [path]                 list directory\n"
         "  rm <path>                 remove file / empty directory\n"
         "  mv <from> <to>            rename / move\n"
         "  stat <path>               inode details (extent map)\n"
         "  stats                     allocation & fragmentation stats\n"
         "  import <hostfile> <path>  copy host file into image\n"
         "  export <path> <hostfile>  copy file out of image\n"
         "  df                        filesystem info\n"
         "  help                      this text\n"
         "  exit                      quit");
}

static int shell_loop(sfs_fs_t *fs)
{
    char line[8192];
    int tty = isatty(STDIN_FILENO);
    for (;;) {
        if (tty) { printf("slopfs> "); fflush(stdout); }
        if (!fgets(line, sizeof(line), stdin))
            break;
        char *argv[8];
        int argc = tokenize(line, argv, 8);
        if (argc <= 0) {
            if (argc < 0) fprintf(stderr, "too many arguments\n");
            continue;
        }
        const char *c = argv[0];
        int rc = 0;
        if (!strcmp(c, "exit") || !strcmp(c, "quit")) break;
        else if (!strcmp(c, "help")) help();
        else if (!strcmp(c, "df")) cmd_df(fs);
        else if (!strcmp(c, "stats")) cmd_stats(fs);
        else if (!strcmp(c, "mkdir") && argc == 2) {
            if ((rc = sfs_mkdir(fs, argv[1])))
                fprintf(stderr, "mkdir: %s: %s\n", argv[1], errstr(rc));
        } else if (!strcmp(c, "touch") && argc == 2) {
            if ((rc = sfs_creat(fs, argv[1])))
                fprintf(stderr, "touch: %s: %s\n", argv[1], errstr(rc));
        } else if (!strcmp(c, "write") && argc == 3) {
            do_write(fs, argv[1], argv[2], 0);
        } else if (!strcmp(c, "append") && argc == 3) {
            do_write(fs, argv[1], argv[2], 1);
        } else if ((!strcmp(c, "read") || !strcmp(c, "cat")) && argc == 2) {
            uint8_t *buf; uint64_t len;
            if ((rc = sfs_read_file(fs, argv[1], &buf, &len))) {
                fprintf(stderr, "read: %s: %s\n", argv[1], errstr(rc));
            } else {
                fwrite(buf, 1, len, stdout);
                if (len && buf[len - 1] != '\n') putchar('\n');
                free(buf);
            }
        } else if (!strcmp(c, "ls") && argc <= 2) {
            const char *path = argc == 2 ? argv[1] : "/";
            sfs_inode_t in;
            if ((rc = sfs_stat_path(fs, path, NULL, &in))) {
                fprintf(stderr, "ls: %s: %s\n", path, errstr(rc));
            } else if (in.type != SFS_TYPE_DIR) {
                printf("%s\n", path);
            } else {
                struct ls_arg a = { fs };
                sfs_dir_iterate(fs, &in, ls_cb, &a);
            }
        } else if (!strcmp(c, "rm") && argc == 2) {
            if ((rc = sfs_unlink(fs, argv[1])))
                fprintf(stderr, "rm: %s: %s\n", argv[1], errstr(rc));
        } else if (!strcmp(c, "mv") && argc == 3) {
            if ((rc = sfs_rename(fs, argv[1], argv[2])))
                fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], argv[2],
                        errstr(rc));
        } else if (!strcmp(c, "stat") && argc == 2) {
            cmd_stat(fs, argv[1]);
        } else if (!strcmp(c, "import") && argc == 3) {
            cmd_import(fs, argv[1], argv[2]);
        } else if (!strcmp(c, "export") && argc == 3) {
            cmd_export(fs, argv[1], argv[2]);
        } else {
            fprintf(stderr, "unknown command (try 'help')\n");
        }
    }
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  slopfs create <disk.img> <size>     e.g. slopfs create disk.img 100MB\n"
        "  slopfs shell  <disk.img>            interactive shell\n"
        "  slopfs stats  <disk.img>            allocation & fragmentation stats\n"
        "  slopfs df     <disk.img>            filesystem info\n"
        "  slopfs replay <disk.img> <seed> [nops]  deterministic replay\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();

    if (!strcmp(argv[1], "create")) {
        if (argc != 4) return usage();
        uint64_t bytes;
        if (parse_size(argv[3], &bytes) != 0) {
            fprintf(stderr, "bad size: %s\n", argv[3]);
            return 2;
        }
        uint64_t nblocks = bytes / SFS_BLOCK_SIZE;
        int rc = sfs_mkfs(argv[2], nblocks);
        if (rc) {
            fprintf(stderr, "create failed: %s\n", errstr(rc));
            return 1;
        }
        printf("created %s: %llu blocks of %u bytes (%llu MB)\n", argv[2],
               (unsigned long long)nblocks, SFS_BLOCK_SIZE,
               (unsigned long long)(bytes >> 20));
        return 0;
    }

    if (!strcmp(argv[1], "shell") || !strcmp(argv[1], "stats") ||
        !strcmp(argv[1], "df")) {
        sfs_fs_t fs;
        int rc = sfs_mount(&fs, argv[2], 0);
        if (rc) {
            fprintf(stderr, "mount failed: %s\n", errstr(rc));
            return 1;
        }
        if (!strcmp(argv[1], "shell"))
            rc = shell_loop(&fs);
        else if (!strcmp(argv[1], "stats"))
            cmd_stats(&fs);
        else
            cmd_df(&fs);
        sfs_unmount(&fs);
        return rc;
    }

    if (!strcmp(argv[1], "replay")) {
        if (argc != 4 && argc != 5) return usage();
        uint64_t seed = strtoull(argv[3], NULL, 0);
        uint32_t nops = argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 0)
                                  : 500;
        int rc = sfs_replay(argv[2], seed, nops);
        if (rc) {
            fprintf(stderr, "replay failed: %s\n", errstr(rc));
            return 1;
        }
        return 0;
    }

    return usage();
}
