#pragma once

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/* ResourceReader: on-demand archive resource access.
 * Instead of extracting entire ZIP/MDD archives to disk at startup,
 * this builds a lightweight filename→archive-entry index and extracts
 * individual files on demand (on first access).
 *
 * The extracted files are cached to a target directory so subsequent
 * accesses are instant (filesystem hit). */

typedef struct ResourceReader ResourceReader;

/* Create a reader for a ZIP file. Scans the central directory, builds index.
 * extract_dir: directory to cache extracted files (will be created if needed).
 * Returns NULL on failure. */
ResourceReader* resource_reader_open_archive(const char *archive_path, const char *extract_dir);

/* Extract a single resource on demand. If the file already exists in
 * extract_dir, returns immediately. Otherwise extracts from archive.
 * Returns the full filesystem path to the extracted file (caller must g_free).
 * Returns NULL if the resource is not found in the archive. */
char* resource_reader_get(ResourceReader *reader, const char *name);

/* Check if a resource exists in the archive. */
gboolean resource_reader_has(ResourceReader *reader, const char *name);

/* Get the extract directory path. */
const char* resource_reader_get_dir(ResourceReader *reader);

/* Free the reader. Does NOT delete extracted files. */
void resource_reader_close(ResourceReader *reader);

/* Create a custom reader with backend-specific callbacks. */
ResourceReader* resource_reader_new(const char *extract_dir, void *backend_data,
                                    char* (*get_func)(ResourceReader*, const char*),
                                    gboolean (*has_func)(ResourceReader*, const char*),
                                    void (*close_func)(ResourceReader*));

/* Get the backend data */
void* resource_reader_get_backend(ResourceReader *reader);
