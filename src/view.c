/*
 * slopfs-view: real-time visual inspector.
 *
 * Opens the image read-only and polls the superblock generation counter
 * (bumped by every committed transaction); on change it reloads bitmaps,
 * re-categorizes every block, and refreshes the directory tree, so each
 * filesystem mutation shows up immediately.
 *
 * Layout:  left  = block map grid (colored cells, aggregated to fit)
 *          right = directory tree (top) + inode inspector (bottom)
 *
 * View modes (keys 1-4):
 *   1  block map             categories: free/meta/journal/data/fragmented
 *   2  extent view           contiguous extents colored per owning file
 *   3  fragmentation heatmap cells colored by extent-break density
 *   4  free space map        free runs colored by contiguous run length
 */
#define _GNU_SOURCE
#include "fs.h"
#include "codec.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* block categories */
enum { CAT_FREE, CAT_META, CAT_JOURNAL, CAT_DATA, CAT_FRAG, CAT_N };
static const char cat_ch[CAT_N]   = { '.', 'M', 'J', '#', '#' };
static const char *cat_name[CAT_N] =
    { "free", "metadata", "journal", "data", "fragmented" };

/* view modes */
enum { MODE_CAT, MODE_EXTENT, MODE_HEAT, MODE_FREE, MODE_N };
static const char *mode_name[MODE_N] = {
    "BLOCK MAP", "EXTENT VIEW", "FRAGMENTATION HEATMAP", "FREE SPACE MAP"
};

/* color pairs */
#define CP(cat)    ((cat) + 1)          /* 1..5: categories            */
#define CP_SEL     7
#define CP_HDR     8
#define NFILECOLOR 6
#define CP_FILE(i) (9 + (i))            /* 9..14: per-file colors      */

typedef struct node {
    char name[SFS_NAME_MAX + 1];
    uint32_t ino;
    uint16_t type;
    int expanded, loaded;
    struct node **child;
    int nchild, depth;
    struct node *parent;
} node_t;

static sfs_fs_t g_fs;
static uint8_t  *g_cat;          /* per-block category                   */
static uint32_t *g_owner;        /* per-block owning inode (0 = none)    */
static uint8_t  *g_break;        /* 1 = first block of discontig extent  */
static uint32_t *g_runlen;       /* length of free run containing block  */
static node_t *g_root;
static node_t **g_vis;           /* flattened visible nodes */
static int g_nvis, g_sel, g_scroll;
static int g_mode = MODE_CAT;
static uint64_t g_gen;

/* ------------------------------------------------------------ block map */

static void cat_mark(uint64_t blk, uint8_t c)
{
    if (blk < g_fs.sb.total_blocks && g_cat[blk] < c)
        g_cat[blk] = c;
}

static void rebuild_categories(void)
{
    const sfs_super_t *sb = &g_fs.sb;
    memset(g_cat, CAT_FREE, sb->total_blocks);
    memset(g_owner, 0, sb->total_blocks * sizeof(uint32_t));
    memset(g_break, 0, sb->total_blocks);
    memset(g_runlen, 0, sb->total_blocks * sizeof(uint32_t));

    for (uint64_t b = 0; b < sb->data_start; b++)
        g_cat[b] = CAT_META;
    for (uint64_t b = sb->journal_start;
         b < sb->journal_start + sb->journal_blocks; b++)
        g_cat[b] = CAT_JOURNAL;

    for (uint32_t ino = 1; ino < sb->inode_count; ino++) {
        if (!((g_fs.imap[ino >> 3] >> (ino & 7)) & 1))
            continue;
        sfs_inode_t in;
        if (sfs_iget(&g_fs, ino, &in) || in.type == SFS_TYPE_FREE)
            continue;
        /* extents are coalesced on append, so adjacent list entries are
         * non-contiguous by construction: nextents > 1 == fragmented */
        uint8_t c = in.nextents > 1 ? CAT_FRAG : CAT_DATA;
        uint32_t prev_end = 0;
        for (uint32_t i = 0; i < in.nextents; i++) {
            sfs_extent_t e;
            if (sfs_ext_get(&g_fs, &in, i, &e))
                break;
            if (i > 0 && e.start != prev_end &&
                e.start < sb->total_blocks)
                g_break[e.start] = 1;
            prev_end = e.start + e.count;
            for (uint32_t b = 0; b < e.count; b++) {
                uint64_t blk = (uint64_t)e.start + b;
                cat_mark(blk, c);
                if (blk < sb->total_blocks)
                    g_owner[blk] = ino;
            }
        }
        if (in.ext_block) {
            cat_mark(in.ext_block, CAT_META);
            if (in.ext_block < sb->total_blocks)
                g_owner[in.ext_block] = ino;
        }
    }

    for (uint32_t i = 0; i < g_fs.nruns; i++) {
        sfs_run_t r = g_fs.runs[i];
        for (uint32_t b = 0; b < r.len; b++)
            if ((uint64_t)r.start + b < sb->total_blocks)
                g_runlen[r.start + b] = r.len;
    }
}

/* decide character + color pair for one aggregated cell [blk, end) */
static void cell_render(uint64_t blk, uint64_t end, chtype *ch, int *cp)
{
    switch (g_mode) {
    case MODE_CAT: {
        uint8_t best = CAT_FREE;
        for (uint64_t b = blk; b < end; b++) {
            /* priority: journal > frag > meta > data > free */
            static const int prio[CAT_N] = { 0, 3, 5, 2, 4 };
            if (prio[g_cat[b]] > prio[best])
                best = g_cat[b];
        }
        *ch = (chtype)cat_ch[best];
        *cp = CP(best);
        return;
    }
    case MODE_EXTENT: {
        uint32_t owner = 0;
        int meta = 0, jrnl = 0;
        for (uint64_t b = blk; b < end; b++) {
            if (g_owner[b]) { owner = g_owner[b]; break; }
            if (g_cat[b] == CAT_JOURNAL) jrnl = 1;
            else if (g_cat[b] == CAT_META) meta = 1;
        }
        if (owner) {
            *ch = '#';
            *cp = CP_FILE(owner % NFILECOLOR);
        } else if (jrnl) {
            *ch = 'J'; *cp = CP(CAT_JOURNAL);
        } else if (meta) {
            *ch = 'M'; *cp = CP(CAT_META);
        } else {
            *ch = '.'; *cp = CP(CAT_FREE);
        }
        return;
    }
    case MODE_HEAT: {
        unsigned breaks = 0;
        int data = 0, meta = 0, jrnl = 0;
        for (uint64_t b = blk; b < end; b++) {
            breaks += g_break[b];
            if (g_cat[b] == CAT_DATA || g_cat[b] == CAT_FRAG) data = 1;
            else if (g_cat[b] == CAT_JOURNAL) jrnl = 1;
            else if (g_cat[b] == CAT_META) meta = 1;
        }
        if (data) {
            *ch = '#';
            *cp = breaks == 0 ? CP(CAT_DATA)
                : breaks <= 2 ? CP(CAT_JOURNAL) : CP(CAT_FRAG);
        } else if (jrnl) {
            *ch = 'J'; *cp = CP(CAT_JOURNAL);
        } else if (meta) {
            *ch = 'M'; *cp = CP(CAT_META);
        } else {
            *ch = '.'; *cp = CP(CAT_FREE);
        }
        return;
    }
    case MODE_FREE:
    default: {
        uint32_t maxrun = 0;
        for (uint64_t b = blk; b < end; b++)
            if (g_runlen[b] > maxrun)
                maxrun = g_runlen[b];
        if (maxrun == 0) {
            *ch = 'x'; *cp = CP(CAT_META);       /* fully used */
        } else if (maxrun >= 256) {
            *ch = '#'; *cp = CP(CAT_DATA);       /* large run  */
        } else if (maxrun >= 32) {
            *ch = '#'; *cp = CP(CAT_JOURNAL);    /* medium run */
        } else {
            *ch = '#'; *cp = CP(CAT_FRAG);       /* small run  */
        }
        return;
    }
    }
}

static void legend_item(int y, int *x, int cp, char ch, const char *label)
{
    attron(COLOR_PAIR(cp));
    mvaddch(y, *x, (chtype)ch);
    attroff(COLOR_PAIR(cp));
    mvprintw(y, *x + 1, " %s  ", label);
    *x += (int)strlen(label) + 4;
}

static void draw_legend(int y, int x0)
{
    int x = x0;
    switch (g_mode) {
    case MODE_CAT:
        for (int i = 0; i < CAT_N; i++)
            legend_item(y, &x, CP(i), cat_ch[i], cat_name[i]);
        break;
    case MODE_EXTENT:
        legend_item(y, &x, CP_FILE(0), '#', "color = owning file");
        legend_item(y, &x, CP(CAT_META), 'M', "metadata");
        legend_item(y, &x, CP(CAT_JOURNAL), 'J', "journal");
        legend_item(y, &x, CP(CAT_FREE), '.', "free");
        break;
    case MODE_HEAT:
        legend_item(y, &x, CP(CAT_DATA), '#', "contiguous");
        legend_item(y, &x, CP(CAT_JOURNAL), '#', "some breaks");
        legend_item(y, &x, CP(CAT_FRAG), '#', "hot (fragmented)");
        legend_item(y, &x, CP(CAT_FREE), '.', "free");
        break;
    case MODE_FREE:
        legend_item(y, &x, CP(CAT_DATA), '#', "run >= 256");
        legend_item(y, &x, CP(CAT_JOURNAL), '#', "run >= 32");
        legend_item(y, &x, CP(CAT_FRAG), '#', "run < 32");
        legend_item(y, &x, CP(CAT_META), 'x', "in use");
        break;
    }
}

static void draw_blockmap(int y0, int x0, int h, int w)
{
    uint64_t total = g_fs.sb.total_blocks;
    uint64_t cells = (uint64_t)h * w;
    uint64_t per = (total + cells - 1) / cells;
    if (per == 0) per = 1;

    attron(COLOR_PAIR(CP_HDR) | A_BOLD);
    mvprintw(y0 - 1, x0, "[%d] %s  (%llu blocks, %llu per cell)",
             g_mode + 1, mode_name[g_mode],
             (unsigned long long)total, (unsigned long long)per);
    attroff(COLOR_PAIR(CP_HDR) | A_BOLD);

    uint64_t blk = 0;
    for (int r = 0; r < h && blk < total; r++) {
        move(y0 + r, x0);
        for (int c = 0; c < w && blk < total; c++) {
            uint64_t end = blk + per;
            if (end > total) end = total;
            chtype ch;
            int cp;
            cell_render(blk, end, &ch, &cp);
            attron(COLOR_PAIR(cp));
            addch(ch);
            attroff(COLOR_PAIR(cp));
            blk = end;
        }
    }
    draw_legend(y0 + h, x0);
}

/* ------------------------------------------------------------ dir tree */

static node_t *node_new(node_t *parent, const char *name, uint32_t ino,
                        uint16_t type)
{
    node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->ino = ino;
    n->type = type;
    n->parent = parent;
    n->depth = parent ? parent->depth + 1 : 0;
    return n;
}

static void node_free_children(node_t *n)
{
    for (int i = 0; i < n->nchild; i++) {
        node_free_children(n->child[i]);
        free(n->child[i]);
    }
    free(n->child);
    n->child = NULL;
    n->nchild = 0;
    n->loaded = 0;
}

struct load_arg { node_t *parent; };
static int load_cb(const sfs_dirent_t *de, void *p)
{
    node_t *parent = ((struct load_arg *)p)->parent;
    sfs_inode_t in;
    uint16_t type = SFS_TYPE_FILE;
    if (sfs_iget(&g_fs, de->ino, &in) == 0)
        type = in.type;
    node_t *n = node_new(parent, de->name, de->ino, type);
    if (!n) return 1;
    node_t **nc = realloc(parent->child,
                          sizeof(node_t *) * (size_t)(parent->nchild + 1));
    if (!nc) { free(n); return 1; }
    parent->child = nc;
    parent->child[parent->nchild++] = n;
    return 0;
}

static int node_cmp(const void *a, const void *b)
{
    const node_t *x = *(node_t *const *)a, *y = *(node_t *const *)b;
    if ((x->type == SFS_TYPE_DIR) != (y->type == SFS_TYPE_DIR))
        return x->type == SFS_TYPE_DIR ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void node_load(node_t *n)
{
    if (n->loaded || n->type != SFS_TYPE_DIR)
        return;
    sfs_inode_t in;
    if (sfs_iget(&g_fs, n->ino, &in) == 0) {
        struct load_arg a = { n };
        sfs_dir_iterate(&g_fs, &in, load_cb, &a);
        qsort(n->child, (size_t)n->nchild, sizeof(node_t *), node_cmp);
    }
    n->loaded = 1;
}

/* reload children of expanded dirs (after a generation change) */
static void node_reload(node_t *n)
{
    if (n->type != SFS_TYPE_DIR || !n->expanded)
        return;
    /* remember which children were expanded, by name */
    int nexp = 0;
    char (*exp)[SFS_NAME_MAX + 1] =
        malloc(sizeof(*exp) * (size_t)(n->nchild ? n->nchild : 1));
    if (exp) {
        for (int i = 0; i < n->nchild; i++)
            if (n->child[i]->expanded)
                snprintf(exp[nexp++], sizeof(exp[0]), "%s",
                         n->child[i]->name);
    }
    node_free_children(n);
    node_load(n);
    if (exp) {
        for (int i = 0; i < n->nchild; i++)
            for (int j = 0; j < nexp; j++)
                if (strcmp(n->child[i]->name, exp[j]) == 0)
                    n->child[i]->expanded = 1;
        free(exp);
    }
    for (int i = 0; i < n->nchild; i++)
        node_reload(n->child[i]);
}

static void flatten(node_t *n)
{
    g_vis = realloc(g_vis, sizeof(node_t *) * (size_t)(g_nvis + 1));
    g_vis[g_nvis++] = n;
    if (n->type == SFS_TYPE_DIR && n->expanded) {
        node_load(n);
        for (int i = 0; i < n->nchild; i++)
            flatten(n->child[i]);
    }
}

static void reflatten(void)
{
    free(g_vis);
    g_vis = NULL;
    g_nvis = 0;
    flatten(g_root);
    if (g_sel >= g_nvis) g_sel = g_nvis - 1;
    if (g_sel < 0) g_sel = 0;
}

static void draw_tree(int y0, int x0, int h, int w)
{
    attron(COLOR_PAIR(CP_HDR) | A_BOLD);
    mvprintw(y0 - 1, x0, "DIRECTORY TREE");
    attroff(COLOR_PAIR(CP_HDR) | A_BOLD);

    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + h) g_scroll = g_sel - h + 1;

    for (int i = 0; i < h; i++) {
        int idx = g_scroll + i;
        if (idx >= g_nvis) break;
        node_t *n = g_vis[idx];
        char line[512];
        const char *mark = n->type == SFS_TYPE_DIR
                               ? (n->expanded ? "[-] " : "[+] ")
                               : "    ";
        snprintf(line, sizeof(line), "%*s%s%s%s", n->depth * 2, "", mark,
                 n->name, n->type == SFS_TYPE_DIR ? "/" : "");
        if (idx == g_sel) attron(COLOR_PAIR(CP_SEL) | A_BOLD);
        mvprintw(y0 + i, x0, "%-*.*s", w, w, line);
        if (idx == g_sel) attroff(COLOR_PAIR(CP_SEL) | A_BOLD);
    }
}

/* ------------------------------------------------------ inode inspector */

static void draw_inspector(int y0, int x0, int h, int w)
{
    (void)h;
    attron(COLOR_PAIR(CP_HDR) | A_BOLD);
    mvprintw(y0 - 1, x0, "INODE INSPECTOR");
    attroff(COLOR_PAIR(CP_HDR) | A_BOLD);

    if (g_nvis == 0) return;
    node_t *n = g_vis[g_sel];
    sfs_inode_t in;
    if (sfs_iget(&g_fs, n->ino, &in)) {
        mvprintw(y0, x0, "inode %u unreadable", n->ino);
        return;
    }

    uint32_t nblk = 0;
    sfs_extent_t exts[8];
    uint32_t nshow = 0;
    for (uint32_t i = 0; i < in.nextents; i++) {
        sfs_extent_t e;
        if (sfs_ext_get(&g_fs, &in, i, &e))
            break;
        nblk += e.count;
        if (nshow < 8) exts[nshow++] = e;
    }
    double frag = (nblk > 1 && in.nextents > 1)
                      ? (double)(in.nextents - 1) / (double)(nblk - 1)
                      : 0.0;

    char ct[32], mt[32], at[32];
    time_t t;
    struct tm tm;
    t = (time_t)in.ctime; localtime_r(&t, &tm);
    strftime(ct, sizeof(ct), "%Y-%m-%d %H:%M:%S", &tm);
    t = (time_t)in.mtime; localtime_r(&t, &tm);
    strftime(mt, sizeof(mt), "%Y-%m-%d %H:%M:%S", &tm);
    t = (time_t)in.atime; localtime_r(&t, &tm);
    strftime(at, sizeof(at), "%Y-%m-%d %H:%M:%S", &tm);

    int y = y0;
    mvprintw(y++, x0, "name: %-32.32s ino: %u",
             n->name, n->ino);
    mvprintw(y++, x0, "type: %-6s size: %llu bytes  links: %u",
             in.type == SFS_TYPE_DIR ? "dir" : "file",
             (unsigned long long)in.size, in.nlinks);
    mvprintw(y++, x0, "extents: %u (%u blocks%s)   frag score: %.2f",
             in.nextents, nblk,
             in.ext_block ? ", overflow block" : "", frag);
    mvprintw(y++, x0, "created : %s", ct);
    mvprintw(y++, x0, "modified: %s", mt);
    mvprintw(y++, x0, "accessed: %s", at);

    char map[256] = "";
    size_t off = 0;
    for (uint32_t i = 0; i < nshow && off < sizeof(map) - 24; i++)
        off += (size_t)snprintf(map + off, sizeof(map) - off, "[%u+%u] ",
                                exts[i].start, exts[i].count);
    if (in.nextents > nshow && off < sizeof(map) - 4)
        snprintf(map + off, sizeof(map) - off, "...");
    mvprintw(y++, x0, "map: %-.*s", w - 5, map);
}

/* --------------------------------------------------------------- main */

static int poll_generation(void)
{
    uint8_t buf[SFS_BLOCK_SIZE];
    sfs_super_t sb;
    if (sfs_dev_read(&g_fs.dev, 0, buf) || sfs_super_decode(&sb, buf))
        return 0;   /* torn concurrent write; retry next tick */
    if (sb.generation == g_gen)
        return 0;
    if (sfs_reload_state(&g_fs))
        return 0;
    g_gen = g_fs.sb.generation;
    rebuild_categories();
    node_reload(g_root);
    reflatten();
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: slopfs-view <disk.img>\n");
        return 2;
    }
    int rc = sfs_mount(&g_fs, argv[1], 1);
    if (rc) {
        fprintf(stderr, "mount failed: %s\n", strerror(-rc));
        return 1;
    }
    g_cat    = calloc(g_fs.sb.total_blocks, 1);
    g_owner  = calloc(g_fs.sb.total_blocks, sizeof(uint32_t));
    g_break  = calloc(g_fs.sb.total_blocks, 1);
    g_runlen = calloc(g_fs.sb.total_blocks, sizeof(uint32_t));
    if (!g_cat || !g_owner || !g_break || !g_runlen) return 1;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(200);   /* poll interval for live updates */
    start_color();
    use_default_colors();
    init_pair(CP(CAT_FREE), COLOR_BLUE, -1);
    init_pair(CP(CAT_META), COLOR_CYAN, -1);
    init_pair(CP(CAT_JOURNAL), COLOR_YELLOW, -1);
    init_pair(CP(CAT_DATA), COLOR_GREEN, -1);
    init_pair(CP(CAT_FRAG), COLOR_RED, -1);
    init_pair(CP_SEL, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_HDR, COLOR_WHITE, -1);
    static const short fcol[NFILECOLOR] = {
        COLOR_GREEN, COLOR_YELLOW, COLOR_MAGENTA,
        COLOR_CYAN, COLOR_RED, COLOR_WHITE
    };
    for (int i = 0; i < NFILECOLOR; i++)
        init_pair(CP_FILE(i), fcol[i], -1);

    g_root = node_new(NULL, "/", g_fs.sb.root_ino, SFS_TYPE_DIR);
    g_root->expanded = 1;
    g_gen = g_fs.sb.generation;
    rebuild_categories();
    reflatten();

    int dirty = 1;
    for (;;) {
        if (dirty) {
            erase();
            int H = LINES, W = COLS;
            int map_w = W * 55 / 100;
            int right_x = map_w + 2;
            int right_w = W - right_x - 1;
            int tree_h = (H - 4) / 2;
            int insp_y = tree_h + 4;

            draw_blockmap(2, 1, H - 5, map_w);
            draw_tree(2, right_x, tree_h, right_w);
            draw_inspector(insp_y, right_x, H - insp_y - 2, right_w);

            attron(A_BOLD);
            mvprintw(0, 1, "slopfs-view %s   gen %llu   free %llu/%llu blocks",
                     argv[1], (unsigned long long)g_gen,
                     (unsigned long long)g_fs.sb.free_blocks,
                     (unsigned long long)g_fs.sb.total_blocks);
            attroff(A_BOLD);
            mvprintw(H - 1, 1,
                     "q quit | 1-4 view mode | up/down select | "
                     "enter/right expand | left collapse | r refresh");
            refresh();
            dirty = 0;
        }

        int ch = getch();
        switch (ch) {
        case 'q':
            goto out;
        case '1': case '2': case '3': case '4':
            g_mode = ch - '1';
            dirty = 1;
            break;
        case KEY_UP:
            if (g_sel > 0) { g_sel--; dirty = 1; }
            break;
        case KEY_DOWN:
            if (g_sel < g_nvis - 1) { g_sel++; dirty = 1; }
            break;
        case '\n': case KEY_ENTER: case KEY_RIGHT: case ' ': {
            node_t *n = g_vis[g_sel];
            if (n->type == SFS_TYPE_DIR) {
                n->expanded = !n->expanded;
                reflatten();
                dirty = 1;
            }
            break;
        }
        case KEY_LEFT: {
            node_t *n = g_vis[g_sel];
            if (n->type == SFS_TYPE_DIR && n->expanded)
                n->expanded = 0;
            else if (n->parent) {
                for (int i = 0; i < g_nvis; i++)
                    if (g_vis[i] == n->parent) { g_sel = i; break; }
            }
            reflatten();
            dirty = 1;
            break;
        }
        case 'r':
            g_gen = 0;   /* force reload on next poll */
            /* fall through */
        case ERR:
        default:
            if (poll_generation())
                dirty = 1;
            break;
        }
    }
out:
    endwin();
    sfs_unmount(&g_fs);
    return 0;
}
