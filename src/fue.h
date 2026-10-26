/* Declarations shared between the scanner (main.c) and the interactive
 * browser (tui.c). */
#ifndef FUE_H
#define FUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* One directory in the breakdown tree.  Every directory at most max_depth
 * levels below the root gets a node; anything deeper is counted towards its
 * nearest ancestor that has one.  Without --depth only the root has a node. */
struct dir_node {
    char *name;     /* heap-owned; the full canonical path for the root, else a basename */
    size_t parent;  /* index of the parent node (unused for the root, node 0) */
    int depth;
    struct stats s; /* this directory's own entries, or its whole subtree once rolled up */
};

struct dir_tree {
    struct dir_node *nodes;
    size_t count;
    size_t cap;
};

/* The children of every node, largest allocated size first: node i's children
 * are order[first[i]] .. order[first[i] + nchildren[i] - 1]. */
struct dir_index {
    size_t *order;
    size_t *first;
    size_t *nchildren;
};

void format_size(uint64_t bytes, char *buf, size_t buf_size);
void format_commas(uint64_t val, char *buf, size_t buf_size);

void dir_tree_path(const struct dir_tree *t, size_t i, char *buf, size_t size);
bool dir_index_build(const struct dir_tree *t, struct dir_index *ix);
void dir_index_free(struct dir_index *ix);

/* Browse a scanned tree interactively (tui.c).  flags_statx is used when
 * listing the files of a directory.  Returns false if the terminal could not
 * be set up. */
bool run_tui(const struct dir_tree *t, int flags_statx);

#endif
