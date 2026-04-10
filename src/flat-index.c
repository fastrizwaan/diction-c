#include "flat-index.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── helpers ──────────────────────────────────────────── */

static int compare_headword(const char *data, const FlatTreeEntry *entry,
                            const char *query, size_t qlen) {
    size_t klen = (size_t)entry->h_len;
    size_t min_len = klen < qlen ? klen : qlen;
    int res = strncasecmp(data + entry->h_off, query, min_len);
    if (res != 0) return res;
    if (klen < qlen) return -1;
    if (klen > qlen) return 1;
    return 0;
}

static int compare_prefix(const char *data, const FlatTreeEntry *entry,
                          const char *prefix, size_t plen) {
    size_t klen = (size_t)entry->h_len;
    size_t cmp_len = klen < plen ? klen : plen;
    int res = strncasecmp(data + entry->h_off, prefix, cmp_len);
    if (res != 0) return res;
    /* If headword is shorter than prefix, it comes before */
    if (klen < plen) return -1;
    return 0; /* headword starts with prefix */
}

/* Comparator for qsort during cache building */
typedef struct {
    const char *data;
} SortCtx;

static __thread const char *sort_data_ptr = NULL;

static int sort_compare(const void *a, const void *b) {
    const FlatTreeEntry *ea = (const FlatTreeEntry *)a;
    const FlatTreeEntry *eb = (const FlatTreeEntry *)b;
    size_t la = (size_t)ea->h_len;
    size_t lb = (size_t)eb->h_len;
    size_t min_len = la < lb ? la : lb;
    int res = strncasecmp(sort_data_ptr + ea->h_off,
                          sort_data_ptr + eb->h_off, min_len);
    if (res != 0) return res;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

/* ── public API ──────────────────────────────────────── */

FlatIndex* flat_index_open(const char *data, size_t size) {
    if (!data || size < 8) return NULL;

    uint64_t count = *(const uint64_t *)data;
    size_t index_size = (size_t)count * sizeof(FlatTreeEntry);

    if (count == 0 || index_size + 8 > size) {
        /* No index or invalid — return empty index */
        FlatIndex *idx = calloc(1, sizeof(FlatIndex));
        if (!idx) return NULL;
        idx->entries = NULL;
        idx->count = 0;
        idx->mmap_data = data;
        idx->mmap_size = size;
        return idx;
    }

    size_t index_off = size - index_size;
    const FlatTreeEntry *entries = (const FlatTreeEntry *)(data + index_off);

    FlatIndex *idx = calloc(1, sizeof(FlatIndex));
    if (!idx) return NULL;
    idx->entries = entries;
    idx->count = (size_t)count;
    idx->mmap_data = data;
    idx->mmap_size = size;
    return idx;
}

void flat_index_close(FlatIndex *idx) {
    free(idx); /* entries are mmap'd, not heap-allocated */
}

size_t flat_index_search(const FlatIndex *idx, const char *query) {
    if (!idx || !idx->entries || idx->count == 0 || !query)
        return (size_t)-1;

    size_t qlen = strlen(query);
    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_headword(idx->mmap_data, &idx->entries[mid], query, qlen);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            result = mid;
            hi = mid; /* find first match */
        }
    }

    return result;
}

size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix) {
    if (!idx || !idx->entries || idx->count == 0 || !prefix)
        return (size_t)-1;

    size_t plen = strlen(prefix);
    if (plen == 0) return 0; /* empty prefix matches everything */

    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_prefix(idx->mmap_data, &idx->entries[mid], prefix, plen);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            result = mid;
            hi = mid; /* find first match */
        }
    }

    return result;
}

const FlatTreeEntry* flat_index_get(const FlatIndex *idx, size_t pos) {
    if (!idx || !idx->entries || pos >= idx->count)
        return NULL;
    return &idx->entries[pos];
}

const FlatTreeEntry* flat_index_successor(const FlatIndex *idx, size_t pos) {
    if (!idx || !idx->entries || pos + 1 >= idx->count)
        return NULL;
    return &idx->entries[pos + 1];
}

const FlatTreeEntry* flat_index_random(const FlatIndex *idx) {
    if (!idx || !idx->entries || idx->count == 0)
        return NULL;
    size_t pos = (size_t)rand() % idx->count;
    return &idx->entries[pos];
}

size_t flat_index_count(const FlatIndex *idx) {
    if (!idx) return 0;
    return idx->count;
}

bool flat_index_validate(const FlatIndex *idx) {
    if (!idx || !idx->entries) return false;

    size_t data_region_end = idx->mmap_size - (idx->count * sizeof(FlatTreeEntry));

    for (size_t i = 0; i < idx->count; i++) {
        int64_t h_off = idx->entries[i].h_off;
        uint64_t h_len = idx->entries[i].h_len;
        int64_t d_off = idx->entries[i].d_off;
        uint64_t d_len = idx->entries[i].d_len;

        if (h_off < 8 || (uint64_t)h_off >= data_region_end) return false;
        if (d_off < 8 || (uint64_t)d_off >= data_region_end) return false;
        if (h_len == 0 || d_len == 0) return false;
        if ((uint64_t)h_off + h_len > data_region_end) return false;
        if ((uint64_t)d_off + d_len > data_region_end) return false;
    }

    return true;
}

void flat_index_sort_entries(FlatTreeEntry *entries, size_t count,
                             const char *data, size_t data_size) {
    (void)data_size;
    if (!entries || count == 0 || !data) return;
    sort_data_ptr = data;
    qsort(entries, count, sizeof(FlatTreeEntry), sort_compare);
    sort_data_ptr = NULL;
}
