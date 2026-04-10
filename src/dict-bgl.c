/* dict-bgl.c — Babylon BGL dictionary parser
 *
 * BGL files begin with a 6-byte header:
 *   bytes 0-3: signature 0x12340001 or 0x12340002
 *   bytes 4-5: offset to gzip stream (big-endian u16)
 *
 * The gzip stream contains typed blocks:
 *   - Type 0: metadata (charset, etc.)
 *   - Type 1,7,10,11: dictionary entries
 *   - Type 3: info blocks (title, author, etc.)
 *   - Type 4: end-of-file
 *
 * We decompress the entire gzip payload into a cache file, then parse
 * the block stream to extract headword→definition pairs into a sorted
 * TreeEntry[] index at the end of the cache, enabling fast binary search
 * on subsequent loads.
 *
 * Reference: goldendict-ng/src/dict/bgl_babylon.cc
 */

#include "dict-mmap.h"
#include "flat-index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#include <glib.h>

/* Use shared cache helpers */
#include "dict-cache.h"

/* Read a big-endian integer of 1-4 bytes from a buffer */
static unsigned int read_be(const unsigned char *p, int bytes) {
    unsigned int val = 0;
    for (int i = 0; i < bytes; i++)
        val = (val << 8) | p[i];
    return val;
}

static char *find_bgl_resource_dir(const char *path) {
    char *base = g_strdup(path);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }

    char *resource_dir = g_strconcat(base, ".files", NULL);
    g_free(base);

    if (g_file_test(resource_dir, G_FILE_TEST_IS_DIR)) {
        return resource_dir;
    }

    g_free(resource_dir);
    return NULL;
}

/* Parse BGL block stream from mmap'd data, building a TreeEntry array.
 * Returns the number of entries found. */
static size_t parse_bgl_blocks(const char *data, size_t data_size,
                               TreeEntry **out_entries, char **out_name,
                               volatile gint *cancel_flag, gint expected) {
    const unsigned char *p = (const unsigned char *)data;
    const unsigned char *end = p + data_size;
    int word_count = 0;

    size_t entry_cap = 4096;
    TreeEntry *entries = calloc(entry_cap, sizeof(TreeEntry));
    char *dict_name = NULL;

    while (p < end) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        if (p + 1 > end) break;

        unsigned int first_byte = p[0];
        unsigned int block_type = first_byte & 0x0F;

        if (block_type == 4) break; /* End of file marker */

        unsigned int len_code = first_byte >> 4;
        p++;

        unsigned int block_len;
        if (len_code < 4) {
            int num_bytes = len_code + 1;
            if (p + num_bytes > end) break;
            block_len = read_be(p, num_bytes);
            p += num_bytes;
        } else {
            block_len = len_code - 4;
        }

        if (block_len == 0 || p + block_len > end) {
            p += block_len;
            continue;
        }

        const unsigned char *block_data = p;
        p += block_len;

        /* Process entry blocks (type 1, 7, 10, 11) */
        if (block_type == 1 || block_type == 7 ||
            block_type == 10 || block_type == 11) {

            unsigned int pos = 0;
            unsigned int hw_len;

            if (block_type == 11) {
                if (pos + 5 > block_len) continue;
                pos = 1;
                hw_len = read_be(block_data + pos, 4);
                pos += 4;
            } else {
                if (pos + 1 > block_len) continue;
                hw_len = block_data[pos++];
            }

            if (pos + hw_len > block_len) continue;

            /* Headword offset/length (relative to data) */
            size_t hw_offset = (const char *)(block_data + pos) - data;
            pos += hw_len;

            /* Definition: skip to definition length */
            unsigned int def_len;
            if (block_type == 11) {
                /* Skip alternate forms count + data */
                if (pos + 4 > block_len) continue;
                unsigned int alts_num = read_be(block_data + pos, 4);
                pos += 4;
                for (unsigned int j = 0; j < alts_num; j++) {
                    if (pos + 4 > block_len) break;
                    unsigned int alt_len = read_be(block_data + pos, 4);
                    pos += 4 + alt_len;
                }
                if (pos + 4 > block_len) continue;
                def_len = read_be(block_data + pos, 4);
                pos += 4;
            } else {
                if (pos + 2 > block_len) continue;
                def_len = read_be(block_data + pos, 2);
                pos += 2;
            }

            if (pos + def_len > block_len) {
                def_len = block_len - pos;
            }

            size_t def_offset = (const char *)(block_data + pos) - data;

            if (hw_len > 0 && def_len > 0) {
                if ((size_t)word_count >= entry_cap) {
                    entry_cap *= 2;
                    entries = realloc(entries, entry_cap * sizeof(TreeEntry));
                }
                entries[word_count].h_off = (int64_t)hw_offset;
                entries[word_count].h_len = (uint64_t)hw_len;
                entries[word_count].d_off = (int64_t)def_offset;
                entries[word_count].d_len = (uint64_t)def_len;
                word_count++;
            }
        } else if (block_type == 3) {
            /* Info block: sub-type 1 is usually the title */
            if (block_len > 1) {
                unsigned int sub_type = block_data[0];
                if (sub_type == 1 && !dict_name) {
                    dict_name = strndup((const char *)block_data + 1, block_len - 1);
                }
            }
        }
    }

    /* Sort entries for binary search */
    if (word_count > 0) {
        flat_index_sort_entries(entries, word_count, data, data_size);
    }

    *out_entries = entries;
    if (out_name) *out_name = dict_name;
    printf("[BGL] Parsed %d entries from source\n", word_count);
    return (size_t)word_count;
}

DictMmap* parse_bgl_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[6];
    if (fread(hdr, 1, 6, f) < 6) { fclose(f); return NULL; }

    /* Verify BGL signature: 0x12 0x34 0x00 0x01|0x02 */
    if (hdr[0] != 0x12 || hdr[1] != 0x34 || hdr[2] != 0x00 ||
        (hdr[3] != 0x01 && hdr[3] != 0x02)) {
        fprintf(stderr, "[BGL] Invalid signature in %s\n", path);
        fclose(f);
        return NULL;
    }

    /* Offset to gzip data */
    int gz_offset = (hdr[4] << 8) | hdr[5];
    if (gz_offset < 6) { fclose(f); return NULL; }

    /* Seek to gz_offset and use gzdopen to read the gzip stream */
    fseek(f, gz_offset, SEEK_SET);
    int fd_dup = dup(fileno(f));
    lseek(fd_dup, gz_offset, SEEK_SET);
    fclose(f);

    gzFile gz = gzdopen(fd_dup, "rb");
    if (!gz) { close(fd_dup); return NULL; }

    // Ensure cache directory exists
    dict_cache_ensure_dir();

    // Get cache path for this dictionary
    char *cache_path = dict_cache_path_for(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && dict_cache_is_valid(cache_path, path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;

    if (cache_valid) {
        /* ── FAST PATH: load from cache with index ── */
        gzclose(gz);
        printf("[BGL] Loading from cache: %s\n", cache_path);
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) {
            g_free(cache_path);
            return NULL;
        }

        struct stat st;
        if (fstat(cache_fd, &st) < 0 || st.st_size < 16) {
            close(cache_fd);
            g_free(cache_path);
            return NULL;
        }
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd);
            g_free(cache_path);
            return NULL;
        }

        DictMmap *dict = calloc(1, sizeof(DictMmap));
        dict->fd = cache_fd;
        dict->tmp_file = NULL;
        dict->data = dict_data;
        dict->size = dict_size;
        dict->resource_dir = find_bgl_resource_dir(path);
        dict->index = flat_index_open(dict->data, dict->size);

        /* Validate the index */
        if (dict->index && dict->index->count > 0) {
            if (!flat_index_validate(dict->index)) {
                fprintf(stderr, "[BGL] Cache index invalid, rebuilding...\n");
                /* Reparse from the cached decompressed data (skip 8-byte header) */
                TreeEntry *entries = NULL;
                char *dict_name = NULL;
                /* The cache data starts with 8-byte count, then the decompressed BGL data */
                size_t data_region_end = dict->size;
                if (dict->index->count > 0) {
                    data_region_end -= dict->index->count * sizeof(TreeEntry);
                }
                size_t bgl_data_start = 8;
                size_t bgl_data_len = data_region_end - bgl_data_start;
                size_t entry_count = parse_bgl_blocks(dict->data + bgl_data_start, bgl_data_len,
                                                      &entries, &dict_name, cancel_flag, expected);
                /* Adjust offsets to account for 8-byte header */
                for (size_t i = 0; i < entry_count; i++) {
                    entries[i].h_off += bgl_data_start;
                    entries[i].d_off += bgl_data_start;
                }
                if (dict_name) dict->name = dict_name;
                free(entries);
                /* Re-create flat index */
                flat_index_close(dict->index);
                dict->index = flat_index_open(dict->data, dict->size);
            } else {
                /* Extract name from the data if we haven't already */
                printf("[BGL] Fast-loaded %zu entries from cache\n", dict->index->count);
            }
        } else {
            /* Cache exists but no index — need to build it */
            fprintf(stderr, "[BGL] Cache has no index, building...\n");
            TreeEntry *entries = NULL;
            char *dict_name = NULL;
            /* Parse starting after the 8-byte header */
            size_t entry_count = parse_bgl_blocks(dict->data + 8, dict->size - 8,
                                                  &entries, &dict_name, cancel_flag, expected);
            if (entry_count > 0 && entries) {
                /* Adjust offsets by +8 for the count header */
                for (size_t i = 0; i < entry_count; i++) {
                    entries[i].h_off += 8;
                    entries[i].d_off += 8;
                }

                /* Upgrade cache: append index, update count */
                int fd_rw = open(cache_path, O_RDWR);
                if (fd_rw >= 0) {
                    lseek(fd_rw, 0, SEEK_END);
                    write(fd_rw, entries, entry_count * sizeof(TreeEntry));
                    lseek(fd_rw, 0, SEEK_SET);
                    uint64_t final_cnt = (uint64_t)entry_count;
                    write(fd_rw, &final_cnt, 8);
                    close(fd_rw);

                    /* Re-mmap */
                    munmap((void*)dict->data, dict->size);
                    struct stat st_new;
                    fstat(dict->fd, &st_new);
                    dict->size = st_new.st_size;
                    dict->data = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
                    flat_index_close(dict->index);
                    dict->index = flat_index_open(dict->data, dict->size);
                    printf("[BGL] Built cache index (%zu entries)\n", entry_count);
                }
            }
            if (dict_name) dict->name = dict_name;
            free(entries);
        }

        g_free(cache_path);
        return dict;
    }

    /* ── BUILD CACHE ── */
    printf("[BGL] Building cache from source: %s\n", path);

    FILE *cache_file = fopen(cache_path, "wb");
    if (!cache_file) {
        gzclose(gz);
        g_free(cache_path);
        return NULL;
    }

    /* Write 8-byte placeholder for entry count */
    uint64_t zero_count = 0;
    fwrite(&zero_count, 8, 1, cache_file);

    unsigned char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
            gzclose(gz);
            fclose(cache_file);
            unlink(cache_path);
            g_free(cache_path);
            return NULL;
        }
        fwrite(buf, 1, n, cache_file);
    }
    gzclose(gz);

    long data_end = ftell(cache_file);
    fclose(cache_file);

    /* Update cache file mtime to match source */
    struct stat src_st;
    if (stat(path, &src_st) == 0) {
        struct utimbuf times;
        times.actime = src_st.st_mtime;
        times.modtime = src_st.st_mtime;
        utime(cache_path, &times);
    }

    /* Open the cache, parse BGL blocks, build index */
    cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) {
        g_free(cache_path);
        return NULL;
    }

    struct stat st;
    if (fstat(cache_fd, &st) < 0 || st.st_size < 16) {
        close(cache_fd);
        g_free(cache_path);
        return NULL;
    }
    dict_size = st.st_size;
    dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (dict_data == MAP_FAILED) {
        close(cache_fd);
        g_free(cache_path);
        return NULL;
    }

    /* Parse blocks from after the 8-byte count header */
    TreeEntry *entries = NULL;
    char *dict_name = NULL;
    size_t entry_count = parse_bgl_blocks(dict_data + 8, dict_size - 8,
                                          &entries, &dict_name, cancel_flag, expected);

    if (entry_count > 0 && entries) {
        /* Adjust offsets by +8 for the count header */
        for (size_t i = 0; i < entry_count; i++) {
            entries[i].h_off += 8;
            entries[i].d_off += 8;
        }

        /* Append index to cache file */
        munmap((void*)dict_data, dict_size);
        close(cache_fd);

        FILE *cf = fopen(cache_path, "r+b");
        if (cf) {
            fseek(cf, data_end, SEEK_SET);
            fwrite(entries, sizeof(TreeEntry), entry_count, cf);
            fseek(cf, 0, SEEK_SET);
            uint64_t final_count = (uint64_t)entry_count;
            fwrite(&final_count, 8, 1, cf);
            fclose(cf);
        }

        /* Re-open for final mmap */
        cache_fd = open(cache_path, O_RDONLY);
        fstat(cache_fd, &st);
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    }

    free(entries);

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = cache_fd;
    dict->tmp_file = NULL;
    dict->data = dict_data;
    dict->size = dict_size;
    dict->name = dict_name;
    dict->resource_dir = find_bgl_resource_dir(path);
    dict->index = flat_index_open(dict->data, dict->size);

    g_free(cache_path);
    printf("[BGL] Loaded %zu entries from %s\n", dict->index ? dict->index->count : 0, path);
    return dict;
}
