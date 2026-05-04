#include "dict-fts-index.h"
#include "dict-loader.h"
#include <stdint.h>
#include <string.h>

struct DictFtsIndex {
    GHashTable *postings; /* uint24 trigram -> GArray<uint32_t> */
    size_t entry_count;
};

static guint32 trigram_key(const unsigned char *p) {
    return ((guint32)p[0] << 16) | ((guint32)p[1] << 8) | (guint32)p[2];
}

static char *fold_text_for_fts(const char *text, gssize len) {
    if (!text) return NULL;
    if (g_utf8_validate(text, len, NULL)) {
        return g_utf8_casefold(text, len);
    }

    if (len < 0) {
        return g_ascii_strdown(text, -1);
    }

    char *copy = g_strndup(text, (gsize)len);
    char *lower = g_ascii_strdown(copy, -1);
    g_free(copy);
    return lower;
}

static void postings_destroy(gpointer data) {
    if (data) {
        g_array_free((GArray *)data, TRUE);
    }
}

static void add_trigrams_for_entry(DictFtsIndex *idx, const char *text, size_t len, guint32 entry_id) {
    if (!text || len < 3) return;

    char *folded = fold_text_for_fts(text, (gssize)len);
    if (!folded) return;

    size_t folded_len = strlen(folded);
    if (folded_len < 3) {
        g_free(folded);
        return;
    }

    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    const unsigned char *bytes = (const unsigned char *)folded;

    for (size_t i = 0; i + 2 < folded_len; i++) {
        guint32 key = trigram_key(bytes + i);
        gpointer gkey = GUINT_TO_POINTER(key);
        if (g_hash_table_contains(seen, gkey)) {
            continue;
        }
        g_hash_table_add(seen, gkey);

        GArray *arr = g_hash_table_lookup(idx->postings, gkey);
        if (!arr) {
            arr = g_array_new(FALSE, FALSE, sizeof(guint32));
            g_hash_table_insert(idx->postings, gkey, arr);
        }
        g_array_append_val(arr, entry_id);
    }

    g_hash_table_destroy(seen);
    g_free(folded);
}

DictFtsIndex* dict_fts_index_build(DictMmap *dict) {
    if (!dict || !dict->index) return NULL;

    DictFtsIndex *idx = g_new0(DictFtsIndex, 1);
    idx->entry_count = flat_index_count(dict->index);
    idx->postings = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, postings_destroy);

    for (size_t i = 0; i < idx->entry_count; i++) {
        const FlatTreeEntry *entry = flat_index_get(dict->index, i);
        if (!entry || entry->d_len == 0) continue;

        char *to_free = NULL;
        size_t def_len = 0;
        const char *def = dict_get_definition(dict, entry, &def_len, &to_free);
        if (def && def_len >= 3) {
            add_trigrams_for_entry(idx, def, def_len, (guint32)i);
        }
        g_free(to_free);
    }

    return idx;
}

static int uint32_compare(gconstpointer a, gconstpointer b) {
    guint32 av = *(const guint32 *)a;
    guint32 bv = *(const guint32 *)b;
    return (av > bv) - (av < bv);
}

static GArray *query_trigrams(const char *query) {
    if (!query) return NULL;

    char *folded = fold_text_for_fts(query, -1);
    if (!folded) return NULL;

    size_t len = strlen(folded);
    if (len < 3) {
        g_free(folded);
        return NULL;
    }

    GArray *keys = g_array_new(FALSE, FALSE, sizeof(guint32));
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    const unsigned char *bytes = (const unsigned char *)folded;

    for (size_t i = 0; i + 2 < len; i++) {
        guint32 key = trigram_key(bytes + i);
        gpointer gkey = GUINT_TO_POINTER(key);
        if (!g_hash_table_contains(seen, gkey)) {
            g_hash_table_add(seen, gkey);
            g_array_append_val(keys, key);
        }
    }

    g_hash_table_destroy(seen);
    g_free(folded);

    if (keys->len == 0) {
        g_array_free(keys, TRUE);
        return NULL;
    }

    g_array_sort(keys, uint32_compare);
    return keys;
}

static gboolean postings_contains_at_or_after(const GArray *arr, guint32 value, guint32 *out_value) {
    guint lo = 0;
    guint hi = arr ? arr->len : 0;

    while (lo < hi) {
        guint mid = lo + (hi - lo) / 2;
        guint32 cur = g_array_index(arr, guint32, mid);
        if (cur < value) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (!arr || lo >= arr->len) return FALSE;
    if (out_value) *out_value = g_array_index(arr, guint32, lo);
    return TRUE;
}

size_t dict_fts_index_search(DictFtsIndex *idx,
                             DictMmap *dict,
                             const char *query,
                             GRegex *regex,
                             size_t start_pos) {
    if (!idx || !dict || !dict->index || !query || !regex) return (size_t)-1;

    GArray *keys = query_trigrams(query);
    if (!keys) return (size_t)-1;

    GArray *smallest = NULL;
    for (guint i = 0; i < keys->len; i++) {
        guint32 key = g_array_index(keys, guint32, i);
        GArray *arr = g_hash_table_lookup(idx->postings, GUINT_TO_POINTER(key));
        if (!arr || arr->len == 0) {
            g_array_free(keys, TRUE);
            return (size_t)-1;
        }
        if (!smallest || arr->len < smallest->len) {
            smallest = arr;
        }
    }

    guint32 candidate = (guint32)start_pos;
    while (postings_contains_at_or_after(smallest, candidate, &candidate)) {
        gboolean present_in_all = TRUE;
        guint32 next_candidate = candidate;

        for (guint i = 0; i < keys->len; i++) {
            guint32 key = g_array_index(keys, guint32, i);
            GArray *arr = g_hash_table_lookup(idx->postings, GUINT_TO_POINTER(key));
            guint32 found = 0;
            if (!postings_contains_at_or_after(arr, candidate, &found)) {
                g_array_free(keys, TRUE);
                return (size_t)-1;
            }
            if (found != candidate) {
                present_in_all = FALSE;
                if (found > next_candidate) next_candidate = found;
            }
        }

        if (!present_in_all) {
            candidate = next_candidate;
            continue;
        }

        const FlatTreeEntry *entry = flat_index_get(dict->index, candidate);
        if (entry && entry->d_len > 0) {
            char *to_free = NULL;
            size_t def_len = 0;
            const char *def = dict_get_definition(dict, entry, &def_len, &to_free);
            gboolean matches = def && g_regex_match_full(regex, def, (gssize)def_len, 0, 0, NULL, NULL);
            g_free(to_free);
            if (matches) {
                g_array_free(keys, TRUE);
                return candidate;
            }
        }

        if (candidate == G_MAXUINT32) break;
        candidate++;
    }

    g_array_free(keys, TRUE);
    return (size_t)-1;
}

void dict_fts_index_free(DictFtsIndex *idx) {
    if (!idx) return;
    g_hash_table_destroy(idx->postings);
    g_free(idx);
}
