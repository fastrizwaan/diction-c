#include "langid.h"
#include <glib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *lang;
    int count;
} LangScore;

static const char *english_stopwords[] = {"the","and","that","have","with","this","from","they","will","would","there","their","what","about","which","when",NULL};
static const char *spanish_stopwords[] = {"que","los","del","las","por","con","para","una","como","pero","sus","este","hay",NULL};
static const char *german_stopwords[]  = {"der","die","und","den","von","das","mit","sich","des","auf","für","ist","nicht","ein","eine",NULL};
static const char *french_stopwords[]  = {"les","des","qui","sur","pas","pour","dans","par","plus","avec","tout","fait","nous","comme",NULL}; 
static const char *italian_stopwords[] = {"che","non","sono","con","una","per","come","della","del","più","gli","alla",NULL};
static const char *portuguese_stopwords[] = {"que","para","com","uma","não","como","mais","dos","sua","são","foi","seus",NULL};
static const char *turkish_stopwords[] = {"bir","için","çok","ile","daha","gibi","sonra","kadar","olan",NULL};

static int count_stopwords(const char *text, const char **stopwords) {
    if (!text || !stopwords) return 0;
    int matches = 0;
    char *lower = g_utf8_strdown(text, -1);
    
    // Simple tokenization by replacing non-alphas with space
    for (int i = 0; lower[i]; i++) {
        if (!g_ascii_isalpha(lower[i]) && ((unsigned char)lower[i] < 128)) {
            lower[i] = ' ';
        }
    }

    // Split by space
    char **tokens = g_strsplit(lower, " ", -1);
    for (int i = 0; tokens && tokens[i]; i++) {
        if (!tokens[i][0]) continue;
        for (int k = 0; stopwords[k]; k++) {
            if (strcmp(tokens[i], stopwords[k]) == 0) {
                matches += strlen(stopwords[k]); // weight longer words more heavily
                break;
            }
        }
    }

    g_strfreev(tokens);
    g_free(lower);
    return matches;
}

const char* langid_guess_language(const char *utf8_text) {
    if (!utf8_text) return "Unknown";

    int script_han = 0;
    int script_hiragana = 0;
    int script_katakana = 0;
    int script_hangul = 0;
    int script_arabic = 0;
    int script_hebrew = 0;
    int script_devanagari = 0;
    int script_cyrillic = 0;
    int script_greek = 0;
    int script_latin = 0;

    const char *p = utf8_text;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        p = g_utf8_next_char(p);

        if (c >= 0x4E00 && c <= 0x9FFF) script_han++;
        else if (c >= 0x3040 && c <= 0x309F) script_hiragana++;
        else if (c >= 0x30A0 && c <= 0x30FF) script_katakana++;
        else if (c >= 0xAC00 && c <= 0xD7AF) script_hangul++;
        else if (c >= 0x0600 && c <= 0x06FF) script_arabic++;
        else if (c >= 0x0590 && c <= 0x05FF) script_hebrew++;
        else if (c >= 0x0900 && c <= 0x097F) script_devanagari++;
        else if (c >= 0x0400 && c <= 0x04FF) script_cyrillic++;
        else if (c >= 0x0370 && c <= 0x03FF) script_greek++;
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0x00C0 && c <= 0x00FF)) script_latin++;
    }

    // High confidence non-Latin scripts
    if (script_hiragana > 10 || script_katakana > 10) return "Japanese";
    if (script_hangul > 10) return "Korean";
    if (script_han > 20) return "Chinese"; // Note: simplified vs traditional is hard, just call it Chinese
    if (script_arabic > 15) return "Arabic";
    if (script_hebrew > 15) return "Hebrew";
    if (script_devanagari > 15) return "Hindi";
    if (script_cyrillic > 15) return "Russian";
    if (script_greek > 15) return "Greek";

    // If latin dominates, fallback to stopword frequency
    if (script_latin > 20) {
        int e_score = count_stopwords(utf8_text, english_stopwords);
        int s_score = count_stopwords(utf8_text, spanish_stopwords);
        int g_score = count_stopwords(utf8_text, german_stopwords);
        int f_score = count_stopwords(utf8_text, french_stopwords);
        int i_score = count_stopwords(utf8_text, italian_stopwords);
        int p_score = count_stopwords(utf8_text, portuguese_stopwords);
        int t_score = count_stopwords(utf8_text, turkish_stopwords);

        LangScore scores[7] = {
            {"English", e_score}, {"Spanish", s_score}, {"German", g_score},
            {"French", f_score}, {"Italian", i_score}, {"Portuguese", p_score},
            {"Turkish", t_score}
        };

        const char *best_lang = "Unknown";
        int max_score = 0;
        for (int i = 0; i < 7; i++) {
            if (scores[i].count > max_score) {
                max_score = scores[i].count;
                best_lang = scores[i].lang;
            }
        }
        
        // Require a minimum threshold to avoid noise from random short words
        if (max_score < 8) {
            return "Unknown";
        }

        return best_lang;
    }

    return "Unknown";
}
