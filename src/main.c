#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Version string is normally injected by the build (see Makefile). */
#ifndef FUE_VERSION
#define FUE_VERSION "unknown"
#endif

/* Fallback definitions for older header versions if necessary */
#ifndef AT_STATX_DONT_SYNC
#define AT_STATX_DONT_SYNC 0x4000
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x0100
#endif

/* Worker thread count.  Traversal is dominated by statx() latency (an MDT
 * round-trip on Lustre), so a modest pool keeps several requests in flight
 * without flooding the metadata server. */
#define DEFAULT_THREADS 8
#define MAX_THREADS     256

/* Deepest --depth accepted; no path that fits in PATH_MAX can be deeper. */
#define MAX_DEPTH (PATH_MAX / 2)

/* Statistics container */
struct stats {
    uint64_t total_size;
    uint64_t total_blocks;
    uint64_t reg_count;
    uint64_t dir_count;
    uint64_t sym_count;
    uint64_t other_count;
    uint64_t perm_denied_count;
    uint64_t total_processed;
};

/* Cache file format identity. Bump CACHE_VERSION whenever the on-disk layout or
 * the semantics of any cached field change, so older incompatible cache files
 * are rejected instead of silently misread under the new layout. */
#define CACHE_MAGIC   0x46554543u   /* "FUEC" */
#define CACHE_VERSION 1u

/* Cache file structure. magic and version are kept first so they sit at a
 * stable offset and stay interpretable even if the rest of the layout changes. */
struct cache_data {
    uint32_t magic;
    uint32_t version;
    char canonical_path[PATH_MAX];
    time_t timestamp;
    uint64_t total_size;
    uint64_t total_blocks;
    uint64_t reg_count;
    uint64_t dir_count;
    uint64_t sym_count;
    uint64_t other_count;
    uint64_t perm_denied_count;
};

/* 64-bit FNV-1a hash used only to derive a cache filename from the canonical
 * path.  This is a fast non-cryptographic hash; collisions are harmless because
 * the cache reader re-compares the full canonical path before trusting a hit. */
uint64_t fnv1a_64(const char *str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/* Format sizes in human-readable representations */
void format_size(uint64_t bytes, char *buf, size_t buf_size) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    int unit_idx = 0;
    double size = (double)bytes;
    while (size >= 1024.0 && unit_idx < 6) {
        size /= 1024.0;
        unit_idx++;
    }
    if (unit_idx == 0) {
        snprintf(buf, buf_size, "%" PRIu64 " B", bytes);
    } else {
        snprintf(buf, buf_size, "%.2f %s", size, units[unit_idx]);
    }
}

/* Helper to format numbers with commas (e.g. 1000 -> 1,000) */
void format_commas(uint64_t val, char *buf, size_t buf_size) {
    char temp[32];
    int len = snprintf(temp, sizeof(temp), "%" PRIu64, val);
    if (len <= 0) {
        buf[0] = '\0';
        return;
    }
    int out_idx = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            if (out_idx < (int)buf_size - 1) {
                buf[out_idx++] = ',';
            }
        }
        if (out_idx < (int)buf_size - 1) {
            buf[out_idx++] = temp[i];
        }
    }
    buf[out_idx] = '\0';
}

/* Format cache age in a readable string */
void format_age(double seconds, char *buf, size_t buf_size) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%.0f second(s)", seconds);
    } else if (seconds < 3600) {
        int minutes = (int)(seconds / 60);
        int secs = (int)seconds % 60;
        snprintf(buf, buf_size, "%d minute(s), %d second(s)", minutes, secs);
    } else {
        int hours = (int)(seconds / 3600);
        int minutes = ((int)seconds % 3600) / 60;
        snprintf(buf, buf_size, "%d hour(s), %d minute(s)", hours, minutes);
    }
}

/* Print results based on execution mode */
void print_results(const char *canonical_path, uint64_t total_size, uint64_t total_blocks,
                   uint64_t reg_count, uint64_t dir_count, uint64_t sym_count, uint64_t other_count,
                   uint64_t perm_denied, bool csv_mode) {
    if (csv_mode) {
        printf("canonical_path,total_size_bytes,total_blocks,reg_files,directories,symlinks,other,perm_denied_errors\n");
        printf("\"%s\",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
               canonical_path,
               total_size,
               total_blocks,
               reg_count,
               dir_count,
               sym_count,
               other_count,
               perm_denied);
    } else {
        char size_buf[64];
        char blocks_buf[64];
        format_size(total_size, size_buf, sizeof(size_buf));
        format_size(total_blocks * 512, blocks_buf, sizeof(blocks_buf));

        printf("--------------------------------------------------\n");
        printf("Disk Usage Analysis Report\n");
        printf("--------------------------------------------------\n");
        printf("Canonical Path:           %s\n", canonical_path);
        printf("Total Apparent Size:      %s (%" PRIu64 " bytes)\n", size_buf, total_size);
        printf("Total Allocated Blocks:   %" PRIu64 " (allocated size: %s)\n", total_blocks, blocks_buf);
        printf("Regular Files Count:      %" PRIu64 "\n", reg_count);
        printf("Directories Count:        %" PRIu64 "\n", dir_count);
        printf("Symbolic Links Count:     %" PRIu64 "\n", sym_count);
        printf("Other Files Count:        %" PRIu64 "\n", other_count);
        printf("Permission Denied Errors: %" PRIu64 "\n", perm_denied);
        printf("--------------------------------------------------\n");
    }
}

/* Add every counter in src into dst. */
static void stats_add(struct stats *dst, const struct stats *src) {
    dst->total_size += src->total_size;
    dst->total_blocks += src->total_blocks;
    dst->reg_count += src->reg_count;
    dst->dir_count += src->dir_count;
    dst->sym_count += src->sym_count;
    dst->other_count += src->other_count;
    dst->perm_denied_count += src->perm_denied_count;
    dst->total_processed += src->total_processed;
}

/* Make room for one more element in a growable array, doubling its capacity
 * when full.  Returns the (possibly moved) array, or NULL on allocation
 * failure, in which case the original array is left untouched. */
static void *grow_array(void *items, size_t *cap, size_t count, size_t elem_size) {
    if (count < *cap) {
        return items;
    }
    size_t new_cap = *cap == 0 ? 64 : *cap * 2;
    void *new_items = realloc(items, new_cap * elem_size);
    if (new_items) {
        *cap = new_cap;
    }
    return new_items;
}

/* A directory still to be visited. */
struct dir_item {
    char *path;  /* heap-owned */
    size_t node; /* breakdown node that this directory's contents count towards */
    int depth;   /* levels below the traversal root, which is depth 0 */
};

/* A growable LIFO stack of directories still to be visited.  Using an explicit
 * stack keeps traversal depth off the C call stack, so deep directory trees
 * can no longer cause a stack overflow. */
struct dir_stack {
    struct dir_item *items;
    size_t count;
    size_t cap;
};

/* Push item onto the stack, taking ownership of its path on success.  Returns
 * false on allocation failure (the caller still owns the path). */
static bool dir_stack_push(struct dir_stack *st, struct dir_item item) {
    struct dir_item *items = grow_array(st->items, &st->cap, st->count, sizeof(*items));
    if (!items) {
        return false;
    }
    st->items = items;
    st->items[st->count++] = item;
    return true;
}

/* Pop the top item (transferring ownership of its path), or return false if empty. */
static bool dir_stack_pop(struct dir_stack *st, struct dir_item *out) {
    if (st->count == 0) {
        return false;
    }
    *out = st->items[--st->count];
    return true;
}

/* One directory in the --depth breakdown.  Every directory at most max_depth
 * levels below the root gets a node; anything deeper is counted towards its
 * nearest ancestor that has one.  Without --depth only the root has a node. */
struct dir_node {
    char *path;     /* heap-owned */
    size_t parent;  /* index of the parent node (unused for the root, node 0) */
    int depth;
    struct stats s; /* this directory's own entries, or its whole subtree once rolled up */
};

struct dir_tree {
    struct dir_node *nodes;
    size_t count;
    size_t cap;
};

/* Append a node holding a copy of path.  Returns its index, or SIZE_MAX on
 * allocation failure. */
static size_t dir_tree_add(struct dir_tree *t, const char *path, size_t parent, int depth,
                           const struct stats *s) {
    struct dir_node *nodes = grow_array(t->nodes, &t->cap, t->count, sizeof(*nodes));
    if (!nodes) {
        return SIZE_MAX;
    }
    t->nodes = nodes;
    char *copy = strdup(path);
    if (!copy) {
        return SIZE_MAX;
    }
    t->nodes[t->count] = (struct dir_node){copy, parent, depth, *s};
    return t->count++;
}

static void dir_tree_free(struct dir_tree *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->nodes[i].path);
    }
    free(t->nodes);
}

/* State shared by all worker threads.  The traversal is finished once the
 * stack is empty and no worker is still scanning a directory (which could
 * otherwise push more work).  stack, tree and active are guarded by lock. */
struct work_queue {
    struct dir_stack stack;
    struct dir_tree tree;
    size_t active;                  /* workers currently scanning a directory */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    atomic_uint_fast64_t processed; /* global entry count, for --progress only */
    int flags_statx;
    bool show_progress;
    int max_depth;
};

/* Count subdirectory name of dir (whose own inode is stx) and queue it on
 * found.  Within the --depth breakdown the subdirectory gets a node of its
 * own, which its inode counts towards; otherwise it is counted into s along
 * with the rest of dir. */
static void visit_subdir(struct work_queue *q, const struct dir_item *dir, const char *name,
                         const struct statx *stx, struct stats *s, struct dir_stack *found) {
    struct stats self = {
        .total_size = stx->stx_size,
        .total_blocks = stx->stx_blocks,
        .dir_count = 1,
        .total_processed = 1,
    };

    char subpath[PATH_MAX];
    int len = snprintf(subpath, sizeof(subpath), "%s/%s", dir->path, name);
    if (len >= (int)sizeof(subpath)) {
        fprintf(stderr, "Warning: Path too long, skipping subdirectory: %s/%s\n", dir->path, name);
        stats_add(s, &self);
        return;
    }

    struct dir_item sub = {.node = dir->node, .depth = dir->depth + 1};
    if (sub.depth <= q->max_depth) {
        pthread_mutex_lock(&q->lock);
        size_t node = dir_tree_add(&q->tree, subpath, dir->node, sub.depth, &self);
        pthread_mutex_unlock(&q->lock);
        if (node != SIZE_MAX) {
            sub.node = node;
        } else {
            fprintf(stderr, "Memory allocation error\n");
            stats_add(s, &self);
        }
    } else {
        stats_add(s, &self);
    }

    /* Queue the subdirectory for a later iteration. */
    sub.path = strdup(subpath);
    if (!sub.path || !dir_stack_push(found, sub)) {
        fprintf(stderr, "Memory allocation error\n");
        free(sub.path);
    }
}

/* Read one directory, counting its entries into s and pushing any
 * subdirectories onto found for a later visit. */
static void scan_directory(struct work_queue *q, const struct dir_item *dir, struct stats *s,
                           struct dir_stack *found) {
    DIR *dirp = opendir(dir->path);
    if (!dirp) {
        if (errno == EACCES) {
            s->perm_denied_count++;
        } else {
            fprintf(stderr, "Error opening directory %s: %s\n", dir->path, strerror(errno));
        }
        return;
    }

    int dfd = dirfd(dirp);
    if (dfd < 0) {
        fprintf(stderr, "Error acquiring file descriptor for %s: %s\n", dir->path, strerror(errno));
        closedir(dirp);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dirp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        struct statx stx;
        if (statx(dfd, entry->d_name, AT_SYMLINK_NOFOLLOW | q->flags_statx,
                  STATX_TYPE | STATX_SIZE | STATX_BLOCKS, &stx) != 0) {
            if (errno == EACCES) {
                s->perm_denied_count++;
            } else {
                fprintf(stderr, "Error querying %s/%s: %s\n", dir->path, entry->d_name, strerror(errno));
            }
            continue;
        }

        if (q->show_progress) {
            uint64_t n = atomic_fetch_add_explicit(&q->processed, 1, memory_order_relaxed) + 1;
            if (n % 100 == 0) {
                char processed_str[32];
                format_commas(n, processed_str, sizeof(processed_str));
                fprintf(stderr, "\rProcessed: %s files/directories...", processed_str);
                fflush(stderr);
            }
        }

        if (S_ISDIR(stx.stx_mode)) {
            visit_subdir(q, dir, entry->d_name, &stx, s, found);
            continue;
        }

        s->total_size += stx.stx_size;
        s->total_blocks += stx.stx_blocks;
        s->total_processed++;

        if (S_ISLNK(stx.stx_mode)) {
            s->sym_count++;
        } else if (S_ISREG(stx.stx_mode)) {
            s->reg_count++;
        } else {
            s->other_count++;
        }
    }
    closedir(dirp); /* Closed immediately to minimize concurrent FD usage */
}

/* Worker loop: repeatedly take a directory from the shared queue, scan it,
 * then hand its totals and any subdirectories it found back in one batch. */
static void *traverse_worker(void *arg) {
    struct work_queue *q = arg;
    struct dir_stack found = {0};

    for (;;) {
        struct dir_item dir;
        pthread_mutex_lock(&q->lock);
        while (q->stack.count == 0 && q->active > 0) {
            pthread_cond_wait(&q->cond, &q->lock);
        }
        if (!dir_stack_pop(&q->stack, &dir)) {
            /* Nothing queued and nobody left to queue more: we are done. */
            pthread_mutex_unlock(&q->lock);
            break;
        }
        q->active++;
        pthread_mutex_unlock(&q->lock);

        struct stats s = {0};
        scan_directory(q, &dir, &s, &found);
        free(dir.path);

        pthread_mutex_lock(&q->lock);
        stats_add(&q->tree.nodes[dir.node].s, &s);
        size_t new_work = found.count;
        for (size_t i = 0; i < found.count; i++) {
            if (!dir_stack_push(&q->stack, found.items[i])) {
                fprintf(stderr, "Memory allocation error\n");
                free(found.items[i].path);
            }
        }
        found.count = 0;
        q->active--;
        /* Wake idle workers for the new directories, or so they can all
         * observe that the traversal has just finished. */
        if (new_work > 0 || q->active == 0) {
            pthread_cond_broadcast(&q->cond);
        }
        pthread_mutex_unlock(&q->lock);
    }

    free(found.items);
    return NULL;
}

/* Parallel iterative traversal of the directory root_path, whose own inode is
 * already counted in s.  nthreads workers (the calling thread is one of them)
 * share a stack of pending directories.  On success, tree holds a node for
 * every directory at most max_depth levels below the root (node 0 is the
 * root itself), each carrying the totals of its whole subtree, and s holds the
 * grand total.  The caller must free tree with dir_tree_free(). */
bool traverse(const char *root_path, struct stats *s, int flags_statx, bool show_progress,
              int nthreads, int max_depth, struct dir_tree *tree) {
    struct work_queue q = {0};
    q.flags_statx = flags_statx;
    q.show_progress = show_progress;
    q.max_depth = max_depth;

    /* The root inode is already in s, so the root node starts empty. */
    const struct stats empty = {0};
    pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
    struct dir_item root = {strdup(root_path), 0, 0};
    if (!tids || !root.path || dir_tree_add(&q.tree, root_path, 0, 0, &empty) == SIZE_MAX ||
        !dir_stack_push(&q.stack, root)) {
        fprintf(stderr, "Memory allocation error\n");
        free(tids);
        free(root.path);
        free(q.stack.items);
        dir_tree_free(&q.tree);
        return false;
    }
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.cond, NULL);
    atomic_init(&q.processed, s->total_processed);

    /* Worker 0 runs on this thread; failing to spawn extras only costs speed. */
    int started = 1;
    for (; started < nthreads; started++) {
        int err = pthread_create(&tids[started], NULL, traverse_worker, &q);
        if (err != 0) {
            fprintf(stderr, "Warning: Could only start %d of %d threads: %s\n",
                    started, nthreads, strerror(err));
            break;
        }
    }
    traverse_worker(&q);
    for (int i = 1; i < started; i++) {
        pthread_join(tids[i], NULL);
    }

    /* Roll every node up into its parent.  A node is always added after its
     * parent, so walking backwards completes each subtree before it is added. */
    for (size_t i = q.tree.count - 1; i > 0; i--) {
        stats_add(&q.tree.nodes[q.tree.nodes[i].parent].s, &q.tree.nodes[i].s);
    }
    stats_add(s, &q.tree.nodes[0].s);
    q.tree.nodes[0].s = *s;
    *tree = q.tree;

    free(q.stack.items); /* Workers only exit once the stack is empty. */
    free(tids);
    pthread_cond_destroy(&q.cond);
    pthread_mutex_destroy(&q.lock);
    return true;
}

/* qsort() has no context argument, so cmp_nodes() reads the nodes from here. */
static const struct dir_node *sort_nodes;

/* Order node indices by parent, then by allocated size (largest first), so
 * the children of each node end up contiguous and sorted. */
static int cmp_nodes(const void *a, const void *b) {
    const struct dir_node *x = &sort_nodes[*(const size_t *)a];
    const struct dir_node *y = &sort_nodes[*(const size_t *)b];
    if (x->parent != y->parent) {
        return x->parent < y->parent ? -1 : 1;
    }
    if (x->s.total_blocks != y->s.total_blocks) {
        return x->s.total_blocks > y->s.total_blocks ? -1 : 1;
    }
    return strcmp(x->path, y->path);
}

/* Print the --depth breakdown: every node in depth-first order, each
 * directory's children largest first.  The human-readable form indents names
 * under their parent; CSV gives one row per directory with its full path. */
void print_breakdown(const struct dir_tree *t, bool csv_mode) {
    size_t n = t->count;
    size_t *order = malloc(n * sizeof(*order));     /* non-root nodes, sorted */
    size_t *first = calloc(n, sizeof(*first));      /* position in order of each node's first child */
    size_t *nchildren = calloc(n, sizeof(*nchildren));
    size_t *stack = malloc(n * sizeof(*stack));
    if (!order || !first || !nchildren || !stack) {
        fprintf(stderr, "Memory allocation error\n");
        goto out;
    }

    for (size_t i = 1; i < n; i++) {
        order[i - 1] = i;
    }
    sort_nodes = t->nodes;
    qsort(order, n - 1, sizeof(*order), cmp_nodes);
    for (size_t k = 0; k + 1 < n; k++) {
        size_t parent = t->nodes[order[k]].parent;
        if (nchildren[parent]++ == 0) {
            first[parent] = k;
        }
    }

    if (csv_mode) {
        printf("depth,path,total_size_bytes,total_blocks,reg_files,directories,symlinks,other,perm_denied_errors\n");
    } else {
        printf("%11s  %11s  %11s  %10s  %7s  %s\n",
               "Allocated", "Apparent", "Files", "Dirs", "Denied", "Path");
        printf("%11s  %11s  %11s  %10s  %7s  %s\n",
               "---------", "--------", "-----", "----", "------", "----");
    }

    /* Depth-first walk; push children in reverse so the largest pops first. */
    size_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        size_t i = stack[--top];
        const struct dir_node *d = &t->nodes[i];
        for (size_t c = nchildren[i]; c-- > 0;) {
            stack[top++] = order[first[i] + c];
        }

        if (csv_mode) {
            printf("%d,\"%s\",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   d->depth, d->path, d->s.total_size, d->s.total_blocks, d->s.reg_count,
                   d->s.dir_count, d->s.sym_count, d->s.other_count, d->s.perm_denied_count);
            continue;
        }

        char alloc_buf[64], size_buf[64], files_buf[32], dirs_buf[32], denied_buf[32];
        format_size(d->s.total_blocks * 512, alloc_buf, sizeof(alloc_buf));
        format_size(d->s.total_size, size_buf, sizeof(size_buf));
        format_commas(d->s.reg_count, files_buf, sizeof(files_buf));
        format_commas(d->s.dir_count, dirs_buf, sizeof(dirs_buf));
        format_commas(d->s.perm_denied_count, denied_buf, sizeof(denied_buf));
        const char *name = d->depth == 0 ? d->path : strrchr(d->path, '/') + 1;
        printf("%11s  %11s  %11s  %10s  %7s  %*s%s%s\n",
               alloc_buf, size_buf, files_buf, dirs_buf, denied_buf,
               2 * d->depth, "", name, d->depth > 0 ? "/" : "");
    }

    if (!csv_mode && t->nodes[0].s.perm_denied_count > 0) {
        printf("\nDenied counts entries that could not be read (permission denied);\n"
               "totals that include them are undercounts.\n");
    }

out:
    free(order);
    free(first);
    free(nchildren);
    free(stack);
}

/* Parse an integer option argument.  Returns false unless it is an integer in
 * [min, max]. */
static bool parse_int_option(const char *arg, long min, long max, int *out) {
    char *end;
    errno = 0;
    long val = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || val < min || val > max) {
        return false;
    }
    *out = (int)val;
    return true;
}

int main(int argc, char *argv[]) {
    bool csv_mode = false;
    bool sync_size = false;
    bool no_cache = false;
    bool show_progress = false;
    int nthreads = DEFAULT_THREADS;
    int max_depth = 0;
    const char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0) {
            csv_mode = true;
        } else if (strcmp(argv[i], "--sync-size") == 0) {
            sync_size = true;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            no_cache = true;
        } else if (strcmp(argv[i], "--progress") == 0) {
            show_progress = true;
        } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc || !parse_int_option(argv[i + 1], 1, MAX_THREADS, &nthreads)) {
                fprintf(stderr, "Error: %s requires a thread count from 1 to %d.\n",
                        argv[i], MAX_THREADS);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--depth") == 0) {
            if (i + 1 >= argc || !parse_int_option(argv[i + 1], 1, MAX_DEPTH, &max_depth)) {
                fprintf(stderr, "Error: %s requires a depth from 1 to %d.\n", argv[i], MAX_DEPTH);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("fue %s\n", FUE_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Disk Usage Analyzer (optimized via statx)\n");
            printf("Usage: %s [options] <path>\n\n", argv[0]);
            printf("Options:\n");
            printf("  --csv            Output in parsable CSV format\n");
            printf("  --sync-size      Toggle AT_STATX_DONT_SYNC off for accurate synchronization\n");
            printf("  --no-cache       Do not read cache files\n");
            printf("  --progress       Display a live file counter\n");
            printf("  -j, --threads N  Scan with N parallel threads (default %d)\n", DEFAULT_THREADS);
            printf("  -d, --depth N    Break usage down by directory, N levels deep\n");
            printf("  --version        Show version information\n");
            printf("  -h, --help       Show this message\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        } else {
            if (input_path != NULL) {
                fprintf(stderr, "Error: Multiple target paths are not supported.\n");
                return 1;
            }
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "Error: Missing target path parameter.\n");
        return 1;
    }

    /* Standardize path format using realpath() */
    char canonical_path[PATH_MAX];
    if (realpath(input_path, canonical_path) == NULL) {
        fprintf(stderr, "Error resolving path '%s': %s\n", input_path, strerror(errno));
        return 1;
    }

    /* Establish statx control flags */
    int flags_statx = 0;
    if (!sync_size) {
        flags_statx |= AT_STATX_DONT_SYNC;
    }

    /* Cache processing */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) {
        tmpdir = "/tmp";
    }

    uint64_t path_hash = fnv1a_64(canonical_path);
    char cache_filepath[PATH_MAX];
    snprintf(cache_filepath, sizeof(cache_filepath), "%s/fue_cache_%016llx.bin", tmpdir, (unsigned long long)path_hash);

    /* The cache only holds totals, so a --depth breakdown always rescans. */
    if (!no_cache && max_depth == 0) {
        /* Open securely using O_NOFOLLOW to mitigate symlink vulnerabilities */
        int fd = open(cache_filepath, O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) {
            struct cache_data cached_val;
            if (read(fd, &cached_val, sizeof(struct cache_data)) == sizeof(struct cache_data)) {
                if (cached_val.magic == CACHE_MAGIC && cached_val.version == CACHE_VERSION &&
                    strcmp(cached_val.canonical_path, canonical_path) == 0) {
                    time_t now = time(NULL);
                    double age_seconds = difftime(now, cached_val.timestamp);
                    char age_str[128];
                    format_age(age_seconds, age_str, sizeof(age_str));

                    if (csv_mode) {
                        fprintf(stderr, "Cache hit. Cache age: %s\n", age_str);
                    } else {
                        printf("Cache loaded. Cache age: %s\n", age_str);
                    }

                    print_results(cached_val.canonical_path, cached_val.total_size, cached_val.total_blocks,
                                  cached_val.reg_count, cached_val.dir_count, cached_val.sym_count,
                                  cached_val.other_count, cached_val.perm_denied_count, csv_mode);
                    close(fd);
                    return 0;
                }
            }
            close(fd);
        }
    }

    /* Traversal Execution */
    struct stats s = {0};
    struct dir_tree tree = {0};
    struct statx target_stx;
    if (statx(AT_FDCWD, canonical_path, AT_SYMLINK_NOFOLLOW | flags_statx, 
              STATX_TYPE | STATX_SIZE | STATX_BLOCKS, &target_stx) != 0) {
        if (errno == EACCES) {
            s.perm_denied_count++;
            print_results(canonical_path, 0, 0, 0, 0, 0, 0, s.perm_denied_count, csv_mode);
            return 0;
        } else {
            fprintf(stderr, "Error reading target path: %s\n", strerror(errno));
            return 1;
        }
    }

    s.total_size += target_stx.stx_size;
    s.total_blocks += target_stx.stx_blocks;
    s.total_processed++;

    if (S_ISLNK(target_stx.stx_mode)) {
        s.sym_count++;
    } else if (S_ISREG(target_stx.stx_mode)) {
        s.reg_count++;
    } else if (S_ISDIR(target_stx.stx_mode)) {
        s.dir_count++;
        if (!traverse(canonical_path, &s, flags_statx, show_progress, nthreads, max_depth, &tree)) {
            return 1;
        }
    } else {
        s.other_count++;
    }

    if (show_progress) {
        /* Erase the progress status line cleanly in stderr */
        fprintf(stderr, "\r\033[K");
        fflush(stderr);
    }

    /* Output Final Results */
    if (max_depth > 0 && tree.count > 0) {
        print_breakdown(&tree, csv_mode);
    } else {
        print_results(canonical_path, s.total_size, s.total_blocks, s.reg_count, 
                      s.dir_count, s.sym_count, s.other_count, s.perm_denied_count, csv_mode);
    }
    dir_tree_free(&tree);

    /* Write Cache results */
    struct cache_data cache;
    cache.magic = CACHE_MAGIC;
    cache.version = CACHE_VERSION;
    strncpy(cache.canonical_path, canonical_path, PATH_MAX - 1);
    cache.canonical_path[PATH_MAX - 1] = '\0';
    cache.timestamp = time(NULL);
    cache.total_size = s.total_size;
    cache.total_blocks = s.total_blocks;
    cache.reg_count = s.reg_count;
    cache.dir_count = s.dir_count;
    cache.sym_count = s.sym_count;
    cache.other_count = s.other_count;
    cache.perm_denied_count = s.perm_denied_count;

    /* Save cache files with strict user-only read/write privileges (0600) */
    int cache_fd = open(cache_filepath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (cache_fd >= 0) {
        if (write(cache_fd, &cache, sizeof(struct cache_data)) != sizeof(struct cache_data)) {
            fprintf(stderr, "Warning: Failed to successfully write cache to file.\n");
        }
        close(cache_fd);
    }

    return 0;
}
