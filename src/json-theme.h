#ifndef JSON_THEME_H
#define JSON_THEME_H

#include "dict-render.h"
#include <glib.h>

void json_theme_manager_init(void);
void json_theme_manager_cleanup(void);

int json_theme_get_count(void);
const char* json_theme_get_name(int index);

/* Populates out_palette with string pointers managed by json_theme. 
   Returns TRUE if theme was found, FALSE otherwise. */
gboolean json_theme_get_palette_by_name(const char *name, dsl_theme_palette *out_palette);

#endif
