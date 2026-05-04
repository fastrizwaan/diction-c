#pragma once

#include "dict-mmap.h"
#include <glib.h>
#include <stddef.h>

typedef struct DictFtsIndex DictFtsIndex;

DictFtsIndex* dict_fts_index_build(DictMmap *dict);
size_t dict_fts_index_search(DictFtsIndex *idx,
                             DictMmap *dict,
                             const char *query,
                             GRegex *regex,
                             size_t start_pos);
void dict_fts_index_free(DictFtsIndex *idx);
