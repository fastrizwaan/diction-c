#pragma once

#include <stddef.h>

/* Guesses the language of a given text block based on Unicode 
 * character ranges and a small list of stopwords for Latin texts.
 * Returns a static string of the language name (e.g., "English", "Russian", "Arabic"),
 * or "Unknown" if it couldn't reliably guess. */
const char* langid_guess_language(const char *utf8_text);
