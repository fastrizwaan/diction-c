#pragma once

#include <stddef.h>

typedef struct SplayNode {
    struct SplayNode *left;
    struct SplayNode *right;
    struct SplayNode *parent;
    size_t key_offset;
    size_t key_length;
    size_t val_offset;
    size_t val_length;
} SplayNode;

typedef struct SplayTree {
    SplayNode *root;
    const char *mmap_data;
    size_t mmap_size;
    size_t node_count;
} SplayTree;

SplayTree* splay_tree_new(const char *mmap_data, size_t mmap_size);
void splay_tree_insert(SplayTree *tree, size_t key_offset, size_t key_length, size_t val_offset, size_t val_length);

// Searches for an exact match
SplayNode* splay_tree_search(SplayTree *tree, const char *query);

// Searches for the first match (useful for multi-match duplicates)
SplayNode* splay_tree_search_first(SplayTree *tree, const char *query);

// Helper to iterate or get partial matches if needed
SplayNode* splay_tree_min(SplayNode *node);
SplayNode* splay_tree_successor(SplayNode *node);
SplayNode* splay_tree_get_random(SplayTree *tree);

void splay_tree_free(SplayTree *tree);
