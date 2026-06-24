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

/* A growable LIFO stack of heap-owned directory paths still to be visited.
 * Using an explicit stack keeps traversal depth off the C call stack, so deep
 * directory trees can no longer cause a stack overflow. */
struct dir_stack {
    char **items;
    size_t count;
    size_t cap;
};

/* Push a copy of path onto the stack.  Returns false on allocation failure. */
static bool dir_stack_push(struct dir_stack *st, const char *path) {
    if (st->count >= st->cap) {
        size_t new_cap = st->cap == 0 ? 64 : st->cap * 2;
        char **new_items = realloc(st->items, new_cap * sizeof(char *));
        if (!new_items) {
            return false;
        }
        st->items = new_items;
        st->cap = new_cap;
    }
    char *copy = strdup(path);
    if (!copy) {
        return false;
    }
    st->items[st->count++] = copy;
    return true;
}

/* Pop the top path (transferring ownership to the caller), or NULL if empty. */
static char *dir_stack_pop(struct dir_stack *st) {
    if (st->count == 0) {
        return NULL;
    }
    return st->items[--st->count];
}

/* Iterative traversal logic.  Visits the tree rooted at root_path depth-first
 * using an explicit stack rather than recursion. */
void traverse(const char *root_path, struct stats *s, int flags_statx, bool show_progress) {
    struct dir_stack stack = {0};
    if (!dir_stack_push(&stack, root_path)) {
        fprintf(stderr, "Memory allocation error\n");
        return;
    }

    char *path;
    while ((path = dir_stack_pop(&stack)) != NULL) {
        DIR *dir = opendir(path);
        if (!dir) {
            if (errno == EACCES) {
                s->perm_denied_count++;
            } else {
                fprintf(stderr, "Error opening directory %s: %s\n", path, strerror(errno));
            }
            free(path);
            continue;
        }

        int dfd = dirfd(dir);
        if (dfd < 0) {
            fprintf(stderr, "Error acquiring file descriptor for %s: %s\n", path, strerror(errno));
            closedir(dir);
            free(path);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            struct statx stx;
            if (statx(dfd, entry->d_name, AT_SYMLINK_NOFOLLOW | flags_statx,
                      STATX_TYPE | STATX_SIZE | STATX_BLOCKS, &stx) != 0) {
                if (errno == EACCES) {
                    s->perm_denied_count++;
                } else {
                    fprintf(stderr, "Error querying %s/%s: %s\n", path, entry->d_name, strerror(errno));
                }
                continue;
            }

            s->total_size += stx.stx_size;
            s->total_blocks += stx.stx_blocks;
            s->total_processed++;

            if (show_progress && (s->total_processed % 100 == 0)) {
                char processed_str[32];
                format_commas(s->total_processed, processed_str, sizeof(processed_str));
                fprintf(stderr, "\rProcessed: %s files/directories...", processed_str);
                fflush(stderr);
            }

            if (S_ISLNK(stx.stx_mode)) {
                s->sym_count++;
            } else if (S_ISREG(stx.stx_mode)) {
                s->reg_count++;
            } else if (S_ISDIR(stx.stx_mode)) {
                s->dir_count++;

                /* Build the child path and queue it for a later iteration. */
                char subpath[PATH_MAX];
                int len = snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
                if (len >= (int)sizeof(subpath)) {
                    fprintf(stderr, "Warning: Path too long, skipping subdirectory: %s/%s\n",
                            path, entry->d_name);
                    continue;
                }
                if (!dir_stack_push(&stack, subpath)) {
                    fprintf(stderr, "Memory allocation error\n");
                }
            } else {
                s->other_count++;
            }
        }
        closedir(dir); /* Closed immediately to minimize concurrent FD usage */
        free(path);
    }

    /* Free any paths still queued (only reachable on allocation failure). */
    for (size_t i = 0; i < stack.count; i++) {
        free(stack.items[i]);
    }
    free(stack.items);
}

int main(int argc, char *argv[]) {
    bool csv_mode = false;
    bool sync_size = false;
    bool no_cache = false;
    bool show_progress = false;
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
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("fue %s\n", FUE_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Disk Usage Analyzer (optimized via statx)\n");
            printf("Usage: %s [options] <path>\n\n", argv[0]);
            printf("Options:\n");
            printf("  --csv          Output in parsable CSV format\n");
            printf("  --sync-size    Toggle AT_STATX_DONT_SYNC off for accurate synchronization\n");
            printf("  --no-cache     Do not read cache files\n");
            printf("  --progress     Display a live file counter\n");
            printf("  --version      Show version information\n");
            printf("  -h, --help     Show this message\n");
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

    if (!no_cache) {
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
        traverse(canonical_path, &s, flags_statx, show_progress);
    } else {
        s.other_count++;
    }

    if (show_progress) {
        /* Erase the progress status line cleanly in stderr */
        fprintf(stderr, "\r\033[K");
        fflush(stderr);
    }

    /* Output Final Results */
    print_results(canonical_path, s.total_size, s.total_blocks, s.reg_count, 
                  s.dir_count, s.sym_count, s.other_count, s.perm_denied_count, csv_mode);

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
