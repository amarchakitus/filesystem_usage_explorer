/* Interactive, read-only browser for a scanned tree (fue --tui).
 *
 * Directory totals come from the scan.  Files are not kept during the scan
 * (on a filesystem with tens of millions of files that would cost gigabytes);
 * instead a directory's files are listed the first time it is opened.  There
 * is deliberately no way to delete, modify or run anything from here. */
#define _GNU_SOURCE
#define NCURSES_WIDECHAR 1

#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wchar.h>

#include "fue.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef FUE_VERSION
#define FUE_VERSION "unknown"
#endif

#define BAR_WIDTH 10

enum entry_type { ENTRY_PARENT, ENTRY_DIR, ENTRY_FILE, ENTRY_LINK, ENTRY_OTHER };

/* One row of a directory listing. */
struct entry {
    const char *name;     /* owned by the tree (directories) or a file_list (files) */
    size_t node;          /* tree node of a directory or "..", else SIZE_MAX */
    enum entry_type type;
    uint64_t size;        /* apparent bytes */
    uint64_t blocks;      /* 512-byte blocks */
};

/* The non-directory entries of one directory, read when it is first opened. */
struct file_list {
    struct entry *items;  /* names are heap-owned */
    size_t count;
    size_t cap;
    uint64_t unreadable;  /* entries statx() could not read */
    int error;            /* errno if the directory itself could not be read, else 0 */
};

enum sort_key { SORT_SIZE, SORT_NAME, SORT_ITEMS };

struct tui {
    const struct dir_tree *t;
    struct dir_index ix;
    struct file_list **files; /* per node, NULL until the directory is first opened */
    int flags_statx;

    size_t cur;               /* node being listed */
    struct entry *rows;
    size_t nrows;
    size_t sel;               /* selected row */
    size_t top;               /* first visible row */

    bool apparent;            /* measure by apparent size instead of allocated */
    enum sort_key sort;
};

static uint64_t entry_bytes(const struct tui *ui, const struct entry *e) {
    return ui->apparent ? e->size : e->blocks * 512;
}

/* Number of entries below a directory row (0 for anything else). */
static uint64_t entry_items(const struct tui *ui, const struct entry *e) {
    if (e->type != ENTRY_DIR) {
        return 0;
    }
    const struct stats *s = &ui->t->nodes[e->node].s;
    return s->reg_count + s->dir_count + s->sym_count + s->other_count - 1;
}

static uint64_t entry_denied(const struct tui *ui, const struct entry *e) {
    return e->type == ENTRY_DIR ? ui->t->nodes[e->node].s.perm_denied_count : 0;
}

/* List the files of node's directory, or return the copy read earlier.
 * Returns NULL only on allocation failure. */
static struct file_list *load_files(struct tui *ui, size_t node) {
    if (ui->files[node]) {
        return ui->files[node];
    }
    struct file_list *fl = calloc(1, sizeof(*fl));
    if (!fl) {
        return NULL;
    }
    ui->files[node] = fl;

    char path[PATH_MAX];
    dir_tree_path(ui->t, node, path, sizeof(path));
    DIR *dir = opendir(path);
    if (!dir) {
        fl->error = errno;
        return fl;
    }

    int dfd = dirfd(dir);
    size_t seen = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (++seen % 1000 == 0) {
            mvprintw(LINES - 1, 0, " Reading directory: %zu entries...", seen);
            clrtoeol();
            refresh();
        }
        /* Subdirectories come from the scan, so skip them without a statx(). */
        if (de->d_type == DT_DIR) {
            continue;
        }

        struct statx stx;
        if (statx(dfd, de->d_name, AT_SYMLINK_NOFOLLOW | ui->flags_statx,
                  STATX_TYPE | STATX_SIZE | STATX_BLOCKS, &stx) != 0) {
            fl->unreadable++;
            continue;
        }
        if (S_ISDIR(stx.stx_mode)) {
            continue;
        }

        if (fl->count == fl->cap) {
            size_t new_cap = fl->cap ? fl->cap * 2 : 64;
            struct entry *items = realloc(fl->items, new_cap * sizeof(*items));
            if (!items) {
                break;
            }
            fl->items = items;
            fl->cap = new_cap;
        }
        char *name = strdup(de->d_name);
        if (!name) {
            break;
        }
        enum entry_type type = S_ISREG(stx.stx_mode) ? ENTRY_FILE
                             : S_ISLNK(stx.stx_mode) ? ENTRY_LINK : ENTRY_OTHER;
        fl->items[fl->count++] = (struct entry){name, SIZE_MAX, type, stx.stx_size, stx.stx_blocks};
    }
    closedir(dir);
    return fl;
}

static void free_file_list(struct file_list *fl) {
    if (!fl) {
        return;
    }
    for (size_t i = 0; i < fl->count; i++) {
        free((char *)fl->items[i].name);
    }
    free(fl->items);
    free(fl);
}

/* qsort() has no context argument, so cmp_rows() reads the settings from here. */
static const struct tui *sort_ui;

static int cmp_rows(const void *a, const void *b) {
    const struct entry *x = a;
    const struct entry *y = b;
    if (sort_ui->sort == SORT_NAME) {
        return strcmp(x->name, y->name);
    }
    if (sort_ui->sort == SORT_ITEMS) {
        uint64_t ix = entry_items(sort_ui, x);
        uint64_t iy = entry_items(sort_ui, y);
        if (ix != iy) {
            return ix > iy ? -1 : 1;
        }
    }
    uint64_t bx = entry_bytes(sort_ui, x);
    uint64_t by = entry_bytes(sort_ui, y);
    if (bx != by) {
        return bx > by ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

/* Sort the rows (keeping ".." first) and keep the same entry selected. */
static void sort_rows(struct tui *ui) {
    const char *selected = ui->nrows > 0 ? ui->rows[ui->sel].name : NULL;
    size_t skip = ui->nrows > 0 && ui->rows[0].type == ENTRY_PARENT ? 1 : 0;
    sort_ui = ui;
    qsort(ui->rows + skip, ui->nrows - skip, sizeof(*ui->rows), cmp_rows);
    for (size_t i = 0; i < ui->nrows; i++) {
        if (ui->rows[i].name == selected) {
            ui->sel = i;
            break;
        }
    }
}

/* Show node's directory, selecting the row for child node select (if any). */
static void open_node(struct tui *ui, size_t node, size_t select) {
    const struct dir_index *ix = &ui->ix;
    struct file_list *fl = load_files(ui, node);
    size_t nfiles = fl ? fl->count : 0;
    struct entry *rows = malloc((1 + ix->nchildren[node] + nfiles) * sizeof(*rows));
    if (!rows) {
        return; /* stay where we are */
    }

    size_t n = 0;
    if (node != 0) {
        rows[n++] = (struct entry){"..", ui->t->nodes[node].parent, ENTRY_PARENT, 0, 0};
    }
    for (size_t c = 0; c < ix->nchildren[node]; c++) {
        size_t child = ix->order[ix->first[node] + c];
        const struct dir_node *d = &ui->t->nodes[child];
        rows[n++] = (struct entry){d->name, child, ENTRY_DIR, d->s.total_size, d->s.total_blocks};
    }
    for (size_t f = 0; f < nfiles; f++) {
        rows[n++] = fl->items[f];
    }

    free(ui->rows);
    ui->rows = rows;
    ui->nrows = n;
    ui->cur = node;
    ui->sel = 0;
    ui->top = 0;
    sort_rows(ui);
    for (size_t i = 0; i < n; i++) {
        if (rows[i].type == ENTRY_DIR && rows[i].node == select) {
            ui->sel = i;
            break;
        }
    }
}

static void go_up(struct tui *ui) {
    if (ui->cur != 0) {
        open_node(ui, ui->t->nodes[ui->cur].parent, ui->cur);
    }
}

/* Draw str at (y, x) in at most width columns.  If it does not fit it is cut
 * short with '~', at the end or, with keep_tail (for paths), at the start.
 * Bytes that are not valid in the locale are shown as '?'. */
static void draw_text_ex(WINDOW *win, int y, int x, const char *str, int width, bool keep_tail) {
    wchar_t buf[PATH_MAX];
    size_t n = 0;
    int total = 0;
    mbstate_t st = {0};
    size_t left = strlen(str);
    for (const char *p = str; left > 0 && n < PATH_MAX;) {
        wchar_t wc;
        size_t len = mbrtowc(&wc, p, left, &st);
        if (len == (size_t)-1 || len == (size_t)-2 || len == 0) {
            wc = L'?';
            len = 1;
            memset(&st, 0, sizeof(st));
        }
        int w = wcwidth(wc);
        if (w < 0) {
            wc = L'?';
            w = 1;
        }
        buf[n++] = wc;
        total += w;
        p += len;
        left -= len;
    }

    wmove(win, y, x);
    if (width <= 0) {
        return;
    }
    size_t start = 0;
    int budget = width;
    if (total > width) {
        budget = width - 1;
        if (keep_tail) {
            /* Skip leading characters until the rest fits after the '~'. */
            int skipped = 0;
            while (start < n && total - skipped > budget) {
                skipped += wcwidth(buf[start++]);
            }
            waddch(win, '~');
        }
    }
    int used = 0;
    for (size_t i = start; i < n; i++) {
        int w = wcwidth(buf[i]);
        if (used + w > budget) {
            break;
        }
        waddnwstr(win, &buf[i], 1);
        used += w;
    }
    if (total > width && !keep_tail) {
        waddch(win, '~');
    }
}

static void draw_text(WINDOW *win, int y, int x, const char *str, int width) {
    draw_text_ex(win, y, x, str, width, false);
}

static void draw(struct tui *ui) {
    const struct dir_node *cur = &ui->t->nodes[ui->cur];
    char path[PATH_MAX], buf[256], alloc_buf[32], size_buf[32], items_buf[32], denied_buf[32];
    erase();

    /* Title bar: the directory being shown. */
    dir_tree_path(ui->t, ui->cur, path, sizeof(path));
    attron(A_REVERSE);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 1, "fue %s  ", FUE_VERSION);
    draw_text_ex(stdscr, 0, getcurx(stdscr), path, COLS - getcurx(stdscr) - 1, true);
    attroff(A_REVERSE);

    /* Totals for this directory. */
    format_size(cur->s.total_blocks * 512, alloc_buf, sizeof(alloc_buf));
    format_size(cur->s.total_size, size_buf, sizeof(size_buf));
    format_commas(cur->s.reg_count + cur->s.dir_count + cur->s.sym_count + cur->s.other_count - 1,
                  items_buf, sizeof(items_buf));
    format_commas(cur->s.perm_denied_count, denied_buf, sizeof(denied_buf));
    static const char *sort_names[] = {"size", "name", "items"};
    snprintf(buf, sizeof(buf), " Allocated %s  Apparent %s  Items %s  Denied %s  [sort: %s]",
             alloc_buf, size_buf, items_buf, denied_buf, sort_names[ui->sort]);
    draw_text(stdscr, 1, 0, buf, COLS);

    /* Column headings; the size that bars and shares are based on is bold. */
    mvprintw(2, 0, " ");
    attron(A_UNDERLINE | (ui->apparent ? 0 : A_BOLD));
    printw("%11s", "Allocated");
    attroff(A_UNDERLINE | A_BOLD);
    printw(" ");
    attron(A_UNDERLINE | (ui->apparent ? A_BOLD : 0));
    printw("%11s", "Apparent");
    attroff(A_UNDERLINE | A_BOLD);
    printw(" ");
    attron(A_UNDERLINE);
    printw("%6s", "Share");
    attroff(A_UNDERLINE);
    printw(" %-*s ", BAR_WIDTH + 2, "");
    attron(A_UNDERLINE);
    printw("%9s", "Items");
    attroff(A_UNDERLINE);
    printw("  ");
    attron(A_UNDERLINE);
    printw("Name");
    attroff(A_UNDERLINE);

    int list_top = 3;
    int view = LINES - list_top - 1;
    if (view < 1) {
        view = 1;
    }
    if (ui->sel < ui->top) {
        ui->top = ui->sel;
    } else if (ui->sel >= ui->top + (size_t)view) {
        ui->top = ui->sel - (size_t)view + 1;
    }

    uint64_t largest = 0;
    for (size_t i = 0; i < ui->nrows; i++) {
        if (ui->rows[i].type != ENTRY_PARENT && entry_bytes(ui, &ui->rows[i]) > largest) {
            largest = entry_bytes(ui, &ui->rows[i]);
        }
    }
    uint64_t total = ui->apparent ? cur->s.total_size : cur->s.total_blocks * 512;

    if (ui->nrows == 0) {
        mvprintw(list_top, 1, "(empty directory)");
    }
    for (int r = 0; r < view && ui->top + (size_t)r < ui->nrows; r++) {
        size_t i = ui->top + (size_t)r;
        const struct entry *e = &ui->rows[i];
        int y = list_top + r;

        if (e->type == ENTRY_PARENT) {
            mvprintw(y, 1, "%11s %11s %6s %-*s %9s  ", "", "", "", BAR_WIDTH + 2, "", "");
        } else {
            uint64_t bytes = entry_bytes(ui, e);
            char share[16], bar[BAR_WIDTH + 1];
            format_size(e->blocks * 512, alloc_buf, sizeof(alloc_buf));
            format_size(e->size, size_buf, sizeof(size_buf));
            snprintf(share, sizeof(share), "%5.1f%%", total ? 100.0 * (double)bytes / (double)total : 0.0);
            int filled = largest ? (int)((double)bytes / (double)largest * BAR_WIDTH + 0.5) : 0;
            memset(bar, '#', (size_t)filled);
            memset(bar + filled, ' ', (size_t)(BAR_WIDTH - filled));
            bar[BAR_WIDTH] = '\0';
            if (e->type == ENTRY_DIR) {
                format_commas(entry_items(ui, e), items_buf, sizeof(items_buf));
            } else {
                items_buf[0] = '\0';
            }
            char flag = entry_denied(ui, e) > 0 ? '!' : e->type == ENTRY_LINK ? '@' : ' ';
            mvprintw(y, 1, "%11s %11s %6s [%s] %9s %c", alloc_buf, size_buf, share, bar,
                     items_buf, flag);
        }

        int name_x = getcurx(stdscr);
        bool is_dir = e->type == ENTRY_DIR || e->type == ENTRY_PARENT;
        if (is_dir) {
            attron(A_BOLD);
        }
        snprintf(path, sizeof(path), "%s%s", e->name, e->type == ENTRY_DIR ? "/" : "");
        draw_text(stdscr, y, name_x, path, COLS - name_x - 1);
        attroff(A_BOLD);
        if (i == ui->sel) {
            mvchgat(y, 0, -1, A_REVERSE | (is_dir ? A_BOLD : 0), 0, NULL);
        }
    }

    /* Status line: problems reading this directory, otherwise the key hints. */
    const struct file_list *fl = ui->files[ui->cur];
    if (fl && fl->error) {
        snprintf(buf, sizeof(buf), " Could not list the files here: %s", strerror(fl->error));
    } else if (fl && fl->unreadable) {
        snprintf(buf, sizeof(buf), " %" PRIu64 " entries here could not be read", fl->unreadable);
    } else {
        snprintf(buf, sizeof(buf), " Enter open  Left back  a toggle size  s/n/c sort  "
                                   "i info  ? help  q quit");
    }
    attron(A_REVERSE);
    mvhline(LINES - 1, 0, ' ', COLS);
    draw_text(stdscr, LINES - 1, 0, buf, COLS);
    attroff(A_REVERSE);
    refresh();
}

/* Show lines in a box in the middle of the screen until a key is pressed.
 * Line tail_line (or -1 for none) keeps its end when it is too long. */
static void popup(const char *title, const char **lines, int nlines, int tail_line) {
    int width = 0;
    for (int i = 0; i < nlines; i++) {
        int len = (int)strlen(lines[i]);
        width = len > width ? len : width;
    }
    width += 4;
    if (width > COLS - 2) {
        width = COLS - 2;
    }
    int height = nlines + 4;
    if (height > LINES) {
        height = LINES;
    }
    if (width < 10 || height < 5) {
        return;
    }

    WINDOW *win = newwin(height, width, (LINES - height) / 2, (COLS - width) / 2);
    if (!win) {
        return;
    }
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " %s ", title);
    for (int i = 0; i < nlines && i < height - 4; i++) {
        draw_text_ex(win, i + 2, 2, lines[i], width - 4, i == tail_line);
    }
    mvwprintw(win, height - 1, width - 22 > 2 ? width - 22 : 2, " press any key ");
    wrefresh(win);
    wgetch(win);
    delwin(win);
    touchwin(stdscr);
}

static void show_info(const struct tui *ui, const struct entry *e) {
    char path[PATH_MAX], alloc_buf[32], size_buf[32], num[32];
    char l_path[PATH_MAX + 16], l_type[64], l_alloc[96], l_size[96], l_ratio[64];
    char l_files[96], l_dirs[96], l_links[96], l_other[96], l_denied[96];
    const char *lines[12];
    int n = 0;

    dir_tree_path(ui->t, ui->cur, path, sizeof(path));
    size_t len = strlen(path);
    snprintf(path + len, sizeof(path) - len, "%s%s", path[len - 1] == '/' ? "" : "/", e->name);
    snprintf(l_path, sizeof(l_path), "Path:       %s", path);
    lines[n++] = l_path;

    static const char *type_names[] = {"directory", "directory", "regular file", "symbolic link", "other"};
    snprintf(l_type, sizeof(l_type), "Type:       %s", type_names[e->type]);
    lines[n++] = l_type;

    format_size(e->blocks * 512, alloc_buf, sizeof(alloc_buf));
    format_size(e->size, size_buf, sizeof(size_buf));
    snprintf(l_alloc, sizeof(l_alloc), "Allocated:  %s (%" PRIu64 " bytes)", alloc_buf, e->blocks * 512);
    snprintf(l_size, sizeof(l_size), "Apparent:   %s (%" PRIu64 " bytes)", size_buf, e->size);
    lines[n++] = l_alloc;
    lines[n++] = l_size;
    if (e->size > 0) {
        snprintf(l_ratio, sizeof(l_ratio), "Allocated / apparent: %.2fx",
                 (double)(e->blocks * 512) / (double)e->size);
        lines[n++] = l_ratio;
    }

    if (e->type == ENTRY_DIR) {
        const struct stats *s = &ui->t->nodes[e->node].s;
        format_commas(s->reg_count, num, sizeof(num));
        snprintf(l_files, sizeof(l_files), "Files:      %s", num);
        format_commas(s->dir_count - 1, num, sizeof(num));
        snprintf(l_dirs, sizeof(l_dirs), "Subdirs:    %s", num);
        format_commas(s->sym_count, num, sizeof(num));
        snprintf(l_links, sizeof(l_links), "Symlinks:   %s", num);
        format_commas(s->other_count, num, sizeof(num));
        snprintf(l_other, sizeof(l_other), "Other:      %s", num);
        format_commas(s->perm_denied_count, num, sizeof(num));
        snprintf(l_denied, sizeof(l_denied), "Denied:     %s%s", num,
                 s->perm_denied_count ? " (totals here are undercounts)" : "");
        lines[n++] = "";
        lines[n++] = l_files;
        lines[n++] = l_dirs;
        lines[n++] = l_links;
        lines[n++] = l_other;
        lines[n++] = l_denied;
    }
    popup("Info", lines, n, 0);
}

static void show_help(void) {
    static const char *lines[] = {
        "Up/Down, j/k       Move",
        "PgUp/PgDn, g/G     Move a page / to the top or bottom",
        "Enter, Right, l    Open directory (or show file info)",
        "Left, Backspace, h Go to the parent directory",
        "a                  Toggle allocated / apparent size",
        "s / n / c          Sort by size / name / item count",
        "i                  Show details of the selected entry",
        "q                  Quit",
        "",
        "Read-only: nothing can be deleted or changed from here.",
        "Allocated is space used on disk (what quotas count);",
        "apparent is the logical file size (what ls -l shows).",
        "! marks directories with unreadable entries, so their",
        "totals are undercounts. @ marks symbolic links.",
        "Directory totals come from the scan; a directory's",
        "files are read when you first open it.",
    };
    popup("Help", lines, (int)(sizeof(lines) / sizeof(lines[0])), -1);
}

bool run_tui(const struct dir_tree *t, int flags_statx) {
    struct tui ui = {.t = t, .flags_statx = flags_statx};
    ui.files = calloc(t->count, sizeof(*ui.files));
    if (!ui.files || !dir_index_build(t, &ui.ix)) {
        free(ui.files);
        return false;
    }

    setlocale(LC_CTYPE, "");
    SCREEN *screen = newterm(NULL, stdout, stdin);
    if (!screen) {
        dir_index_free(&ui.ix);
        free(ui.files);
        return false;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    open_node(&ui, 0, SIZE_MAX);
    for (bool quit = false; !quit;) {
        draw(&ui);
        int page = LINES - 4 > 1 ? LINES - 4 : 1;
        int ch = getch();
        switch (ch) {
        case KEY_UP:
        case 'k':
            if (ui.sel > 0) {
                ui.sel--;
            }
            break;
        case KEY_DOWN:
        case 'j':
            if (ui.sel + 1 < ui.nrows) {
                ui.sel++;
            }
            break;
        case KEY_PPAGE:
            ui.sel = ui.sel > (size_t)page ? ui.sel - (size_t)page : 0;
            break;
        case KEY_NPAGE:
            ui.sel += (size_t)page;
            if (ui.sel >= ui.nrows) {
                ui.sel = ui.nrows > 0 ? ui.nrows - 1 : 0;
            }
            break;
        case KEY_HOME:
        case 'g':
            ui.sel = 0;
            break;
        case KEY_END:
        case 'G':
            ui.sel = ui.nrows > 0 ? ui.nrows - 1 : 0;
            break;
        case KEY_RIGHT:
        case KEY_ENTER:
        case 'l':
        case '\n':
        case '\r':
            if (ui.nrows == 0) {
                break;
            }
            if (ui.rows[ui.sel].type == ENTRY_PARENT) {
                go_up(&ui);
            } else if (ui.rows[ui.sel].type == ENTRY_DIR) {
                open_node(&ui, ui.rows[ui.sel].node, SIZE_MAX);
            } else if (ch != KEY_RIGHT && ch != 'l') {
                show_info(&ui, &ui.rows[ui.sel]);
            }
            break;
        case KEY_LEFT:
        case KEY_BACKSPACE:
        case 'h':
        case 127:
        case 8:
            go_up(&ui);
            break;
        case 'a':
            ui.apparent = !ui.apparent;
            sort_rows(&ui);
            break;
        case 's':
        case 'n':
        case 'c':
            ui.sort = ch == 's' ? SORT_SIZE : ch == 'n' ? SORT_NAME : SORT_ITEMS;
            sort_rows(&ui);
            break;
        case 'i':
            if (ui.nrows > 0 && ui.rows[ui.sel].type != ENTRY_PARENT) {
                show_info(&ui, &ui.rows[ui.sel]);
            }
            break;
        case '?':
            show_help();
            break;
        case 'q':
        case 'Q':
            quit = true;
            break;
        default:
            break; /* includes KEY_RESIZE: just redraw */
        }
    }

    endwin();
    delscreen(screen);
    for (size_t i = 0; i < t->count; i++) {
        free_file_list(ui.files[i]);
    }
    free(ui.files);
    free(ui.rows);
    dir_index_free(&ui.ix);
    return true;
}
