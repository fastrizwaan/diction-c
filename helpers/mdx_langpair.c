// mdx_langpair.c — comprehensive MDX language pair detector
// gcc -O2 mdx_langpair.c -o mdx_langpair
//
// Strategy (in priority order):
//   1. Parse MDX XML header for GeneratedByEngineVersion, Title, Description
//   2. Keyword scan on header text (language names, bilingual markers)
//   3. Script census on first 64 KB of file (Unicode block counting)
//   4. Urdu vs Arabic vs Persian disambiguation via vocabulary markers
//   5. Devanagari: Hindi vs Marathi vs Sanskrit heuristic
//   6. CJK: Chinese vs Japanese (kana presence) vs Korean (Hangul)
//   7. Fallback: ASCII ratio → English

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

// ─── tunables ───────────────────────────────────────────────────────────────
#define HEADER_READ   (64  * 1024)   // bytes read for header / script census
#define BODY_READ     (256 * 1024)   // extra bytes for definition script census
#define SCRIPT_THRESH  8             // min codepoints to claim a script

// ─── language codes (ISO 639-1 style) ───────────────────────────────────────
#define L_EN  "En"
#define L_ZH  "Zh"
#define L_JA  "Ja"
#define L_KO  "Ko"
#define L_AR  "Ar"
#define L_FA  "Fa"   // Persian / Farsi
#define L_UR  "Ur"
#define L_HI  "Hi"
#define L_MR  "Mr"   // Marathi
#define L_BN  "Bn"   // Bengali
#define L_PA  "Pa"   // Punjabi (Gurmukhi)
#define L_TA  "Ta"
#define L_TE  "Te"
#define L_KN  "Kn"   // Kannada
#define L_ML  "Ml"   // Malayalam
#define L_RU  "Ru"
#define L_UK  "Uk"   // Ukrainian
#define L_DE  "De"
#define L_FR  "Fr"
#define L_ES  "Es"
#define L_PT  "Pt"
#define L_IT  "It"
#define L_NL  "Nl"
#define L_PL  "Pl"
#define L_TR  "Tr"
#define L_ID  "Id"
#define L_VI  "Vi"
#define L_TH  "Th"
#define L_HE  "He"
#define L_EL  "El"   // Greek
#define L_SV  "Sv"   // Swedish
#define L_NO  "No"   // Norwegian
#define L_DA  "Da"   // Danish
#define L_FI  "Fi"   // Finnish
#define L_HU  "Hu"   // Hungarian
#define L_CS  "Cs"   // Czech
#define L_SK  "Sk"   // Slovak
#define L_RO  "Ro"   // Romanian
#define L_BG  "Bg"   // Bulgarian
#define L_HR  "Hr"   // Croatian
#define L_SR  "Sr"   // Serbian
#define L_UK  "Uk"
#define L_UNK "??"

// ─── keyword → language table ────────────────────────────────────────────────
typedef struct { const char *kw; const char *code; } KwLang;

static const KwLang kw_table[] = {
    // English variants
    {"english",         L_EN}, {"brit",           L_EN}, {"american",       L_EN},
    // Chinese
    {"chinese",         L_ZH}, {"mandarin",       L_ZH}, {"cantonese",      L_ZH},
    {"simplified",      L_ZH}, {"traditional",    L_ZH}, {"putonghua",      L_ZH},
    // Japanese
    {"japanese",        L_JA}, {"nihongo",        L_JA},
    // Korean
    {"korean",          L_KO}, {"hangul",         L_KO},
    // Arabic / Semitic
    {"arabic",          L_AR}, {"عربي",           L_AR}, {"عربى",           L_AR},
    // Persian / Farsi
    {"persian",         L_FA}, {"farsi",          L_FA}, {"فارسی",          L_FA},
    // Urdu
    {"urdu",            L_UR}, {"اردو",           L_UR},
    // Indic
    {"hindi",           L_HI}, {"हिन्दी",           L_HI},
    {"marathi",         L_MR}, {"bengali",        L_BN}, {"bangla",         L_BN},
    {"punjabi",         L_PA}, {"gujarati",       "Gu"},
    {"tamil",           L_TA}, {"telugu",         L_TE},
    {"kannada",         L_KN}, {"malayalam",      L_ML},
    {"sinhala",         "Si"}, {"nepali",         "Ne"},
    // European
    {"russian",         L_RU}, {"русский",        L_RU},
    {"ukrainian",       L_UK}, {"german",         L_DE}, {"deutsch",        L_DE},
    {"french",          L_FR}, {"français",       L_FR}, {"francais",       L_FR},
    {"spanish",         L_ES}, {"español",        L_ES}, {"espanol",        L_ES},
    {"portuguese",      L_PT}, {"italiano",       L_IT}, {"italian",        L_IT},
    {"dutch",           L_NL}, {"polish",         L_PL}, {"polsk",          L_PL},
    {"turkish",         L_TR}, {"indonesian",     L_ID}, {"malay",          "Ms"},
    {"vietnamese",      L_VI}, {"thai",           L_TH},
    {"hebrew",          L_HE}, {"greek",          L_EL},
    {"swedish",         L_SV}, {"norwegian",      L_NO}, {"danish",         L_DA},
    {"finnish",         L_FI}, {"hungarian",      L_HU},
    {"czech",           L_CS}, {"slovak",         L_SK}, {"romanian",       L_RO},
    {"bulgarian",       L_BG}, {"croatian",       L_HR}, {"serbian",        L_SR},
    // misc
    {"latin",           "La"}, {"esperanto",      "Eo"}, {"swahili",        "Sw"},
    {NULL, NULL}
};

// ─── Unicode block census ─────────────────────────────────────────────────────
typedef struct {
    int cjk_unified;     // 4E00–9FFF  (+ ext A/B etc.)
    int hiragana;        // 3040–309F
    int katakana;        // 30A0–30FF
    int hangul;          // AC00–D7AF  + jamo
    int arabic;          // 0600–06FF
    int arabic_ext;      // 0750–077F, FB50–FDFF, FE70–FEFF
    int hebrew;          // 0590–05FF
    int devanagari;      // 0900–097F
    int bengali;         // 0980–09FF
    int gurmukhi;        // 0A00–0A7F
    int gujarati;        // 0A80–0AFF
    int oriya;           // 0B00–0B7F
    int tamil;           // 0B80–0BFF
    int telugu;          // 0C00–0C7F
    int kannada;         // 0C80–0CFF
    int malayalam;       // 0D00–0D7F
    int sinhala;         // 0D80–0DFF
    int thai;            // 0E00–0E7F
    int cyrillic;        // 0400–04FF
    int greek;           // 0370–03FF
    int latin_ext;       // 00C0–02AF  (accented Latin)
    int ascii_alpha;     // 0041–007A
} ScriptCounts;

static uint32_t next_cp(const unsigned char **p, const unsigned char *end) {
    const unsigned char *s = *p;
    if (s >= end) return 0;

    uint8_t b = *s;
    uint32_t cp;
    int len;

    if (b < 0x80)       { cp = b;                                        len = 1; }
    else if (b < 0xC0)  { (*p)++; return 0xFFFD; }  // continuation byte
    else if (b < 0xE0)  { cp = b & 0x1F; len = 2; }
    else if (b < 0xF0)  { cp = b & 0x0F; len = 3; }
    else if (b < 0xF8)  { cp = b & 0x07; len = 4; }
    else                { (*p)++; return 0xFFFD; }

    for (int i = 1; i < len; i++) {
        if (s + i >= end || (s[i] & 0xC0) != 0x80) { *p = s + 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p = s + len;
    return cp;
}

static void census(const unsigned char *buf, size_t n, ScriptCounts *sc) {
    const unsigned char *p = buf, *end = buf + n;
    memset(sc, 0, sizeof(*sc));

    while (p < end) {
        uint32_t c = next_cp(&p, end);
        if (!c) break;

        if (c >= 0x0041 && c <= 0x007A) sc->ascii_alpha++;
        else if (c >= 0x00C0 && c <= 0x02AF) sc->latin_ext++;
        else if (c >= 0x0370 && c <= 0x03FF) sc->greek++;
        else if (c >= 0x0400 && c <= 0x04FF) sc->cyrillic++;
        else if (c >= 0x0590 && c <= 0x05FF) sc->hebrew++;
        else if (c >= 0x0600 && c <= 0x06FF) sc->arabic++;
        else if ((c >= 0x0750 && c <= 0x077F) ||
                 (c >= 0xFB50 && c <= 0xFDFF) ||
                 (c >= 0xFE70 && c <= 0xFEFF)) sc->arabic_ext++;
        else if (c >= 0x0900 && c <= 0x097F) sc->devanagari++;
        else if (c >= 0x0980 && c <= 0x09FF) sc->bengali++;
        else if (c >= 0x0A00 && c <= 0x0A7F) sc->gurmukhi++;
        else if (c >= 0x0A80 && c <= 0x0AFF) sc->gujarati++;
        else if (c >= 0x0B00 && c <= 0x0B7F) sc->oriya++;
        else if (c >= 0x0B80 && c <= 0x0BFF) sc->tamil++;
        else if (c >= 0x0C00 && c <= 0x0C7F) sc->telugu++;
        else if (c >= 0x0C80 && c <= 0x0CFF) sc->kannada++;
        else if (c >= 0x0D00 && c <= 0x0D7F) sc->malayalam++;
        else if (c >= 0x0D80 && c <= 0x0DFF) sc->sinhala++;
        else if (c >= 0x0E00 && c <= 0x0E7F) sc->thai++;
        else if (c >= 0x1100 && c <= 0x11FF) sc->hangul++;
        else if (c >= 0x3040 && c <= 0x309F) sc->hiragana++;
        else if (c >= 0x30A0 && c <= 0x30FF) sc->katakana++;
        else if ((c >= 0x4E00 && c <= 0x9FFF) ||
                 (c >= 0x3400 && c <= 0x4DBF) ||
                 (c >= 0x20000 && c <= 0x2A6DF)) sc->cjk_unified++;
        else if (c >= 0xAC00 && c <= 0xD7AF) sc->hangul++;
    }
}

// ─── Arabic script disambiguation ─────────────────────────────────────────
// Looks for script-specific extended Arabic codepoints and vocabulary markers
static const char* disambiguate_arabic_script(
        const unsigned char *buf, size_t n, const ScriptCounts *sc) {

    // Urdu-specific extended Arabic letters (ٹ ڈ ڑ ژ ں ہ ے)
    // U+0679, U+0688, U+0691, U+0698, U+06BA, U+06C1, U+06D2
    static const uint32_t urdu_markers[] = {
        0x0679, 0x0688, 0x0691, 0x0698, 0x06BA, 0x06BE, 0x06C1, 0x06D2, 0
    };
    // Persian-specific: پ (0x067E), چ (0x0686), ژ (0x0698), گ (0x06AF)
    static const uint32_t persian_markers[] = {
        0x067E, 0x0686, 0x06AF, 0
    };

    int urdu_score = 0, persian_score = 0;
    const unsigned char *p = buf, *end = buf + n;

    while (p < end) {
        uint32_t c = next_cp(&p, end);
        if (!c) break;
        for (int i = 0; urdu_markers[i]; i++)
            if (c == urdu_markers[i]) { urdu_score++; break; }
        for (int i = 0; persian_markers[i]; i++)
            if (c == persian_markers[i]) { persian_score++; break; }
    }

    // Arabic-extended block is a strong Persian/Urdu indicator
    int ext = sc->arabic_ext;

    if (urdu_score > persian_score && urdu_score >= 3) return L_UR;
    if (persian_score >= 3 || ext > 20)                return L_FA;
    if (urdu_score >= 2)                               return L_UR;
    return L_AR;
}

// ─── Devanagari disambiguation ────────────────────────────────────────────
// Marathi uses ळ (U+0933), Sanskrit uses ॐ (U+0950) etc.
static const char* disambiguate_devanagari(const unsigned char *buf, size_t n) {
    int marathi = 0, sanskrit = 0;
    const unsigned char *p = buf, *end = buf + n;
    while (p < end) {
        uint32_t c = next_cp(&p, end);
        if (c == 0x0933) marathi++;        // ळ
        if (c == 0x0950 || c == 0x0902) sanskrit++;  // ॐ ं
    }
    if (marathi > 5)   return L_MR;
    if (sanskrit > 10) return "Sa";
    return L_HI;
}

// ─── utility: lowercase in place, return pointer ──────────────────────────
static void str_tolower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// ─── extract XML attribute values and text nodes ─────────────────────────
// MDX headers are a single XML tag where all content is in attributes:
//   <Dictionary Title="English-Arabic" Description="..." />
// Plain strip_tags would delete everything. Instead we pull out quoted values
// and also preserve any text nodes (for HTML-style definition bodies).
static void extract_attr_values(char *s) {
    char *out = s;
    const char *p = s;

    while (*p) {
        if (*p == '<') {
            p++;
            // skip past tag name
            while (*p && *p != '>' && *p != '"' && *p != '\'') p++;
            // collect quoted attribute values until end of tag
            while (*p && *p != '>') {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    while (*p && *p != q) *out++ = *p++;
                    *out++ = ' ';
                    if (*p) p++;
                } else {
                    p++;
                }
            }
            if (*p == '>') p++;
        } else {
            // text node between tags — keep as-is
            *out++ = *p++;
        }
    }
    *out = '\0';
}

// ─── UTF-16LE decode helpers ─────────────────────────────────────────────
// Emit a Unicode codepoint as UTF-8 into buf; return bytes written (0 on error)
static int cp_to_utf8(uint32_t cp, char *out, size_t avail) {
    if (avail < 4) return 0;
    if (cp < 0x80)        { out[0] = (char)cp; return 1; }
    if (cp < 0x800)       { out[0] = (char)(0xC0|(cp>>6));  out[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000)     { out[0] = (char)(0xE0|(cp>>12)); out[1] = (char)(0x80|((cp>>6)&0x3F));  out[2] = (char)(0x80|(cp&0x3F)); return 3; }
    if (cp < 0x110000)    { out[0] = (char)(0xF0|(cp>>18)); out[1] = (char)(0x80|((cp>>18)&0x3F)); out[2] = (char)(0x80|((cp>>6)&0x3F));  out[3] = (char)(0x80|(cp&0x3F)); return 4; }
    return 0;
}

// Decode UTF-16LE bytes into a UTF-8 string (handles BMP + surrogate pairs)
static void utf16le_to_utf8(const unsigned char *src, size_t src_len,
                             char *out, size_t out_sz) {
    size_t si = 0, oi = 0;
    // skip optional BOM
    if (src_len >= 2 && src[0] == 0xFF && src[1] == 0xFE) si = 2;

    while (si + 1 < src_len && oi + 4 < out_sz) {
        uint16_t w1 = (uint16_t)(src[si] | (src[si+1] << 8));
        si += 2;
        uint32_t cp;
        if (w1 >= 0xD800 && w1 <= 0xDBFF) {
            // high surrogate — need low surrogate
            if (si + 1 >= src_len) break;
            uint16_t w2 = (uint16_t)(src[si] | (src[si+1] << 8));
            si += 2;
            if (w2 < 0xDC00 || w2 > 0xDFFF) continue; // bad pair
            cp = 0x10000 + ((uint32_t)(w1 - 0xD800) << 10) + (w2 - 0xDC00);
        } else {
            cp = w1;
        }
        int nb = cp_to_utf8(cp, out + oi, out_sz - oi - 1);
        oi += nb;
    }
    out[oi] = '\0';
}

// ─── extract MDX XML header ───────────────────────────────────────────────
// MDict v1: [4-byte BE uint32 = header_len] [UTF-16LE XML] [4-byte checksum]
// MDict v2: [4-byte LE uint32 = header_len] [UTF-16LE XML] [4-byte checksum]
// Detect by trying both and picking whichever gives a valid '<' start.
// Returns body_offset (= 4 + hlen).
static size_t extract_header(const unsigned char *buf, size_t n,
                              char *out, size_t outsz) {
    out[0] = '\0';
    if (n < 8) return 0;

    uint32_t hlen_le = (uint32_t)buf[0]
                     | ((uint32_t)buf[1] << 8)
                     | ((uint32_t)buf[2] << 16)
                     | ((uint32_t)buf[3] << 24);

    uint32_t hlen_be = ((uint32_t)buf[0] << 24)
                     | ((uint32_t)buf[1] << 16)
                     | ((uint32_t)buf[2] << 8)
                     |  (uint32_t)buf[3];

    // Pick whichever makes sense: header must fit in buffer and point at '<'
    uint32_t hlen = 0;
    if (hlen_le > 0 && hlen_le <= (1u<<20) && (size_t)(4+hlen_le) <= n &&
        (buf[4] == '<' || (buf[4] == 0xFF && buf[5] == 0xFE) || buf[5] == 0x00)) {
        hlen = hlen_le;
    } else if (hlen_be > 0 && hlen_be <= (1u<<20) && (size_t)(4+hlen_be) <= n &&
               (buf[4] == '<' || (buf[4] == 0xFF && buf[5] == 0xFE) || buf[5] == 0x00)) {
        hlen = hlen_be;
    }

    if (hlen == 0) {
        // Fallback: treat whole buffer as raw text
        size_t copy = (n < outsz - 1) ? n : outsz - 1;
        memcpy(out, buf, copy);
        out[copy] = '\0';
        return 0;
    }

    const unsigned char *hbuf = buf + 4;

    if ((hbuf[0] == 0xFF && hbuf[1] == 0xFE) ||
        (hbuf[0] == '<'  && hbuf[1] == 0x00)) {
        // UTF-16LE → UTF-8 (full conversion, preserves non-ASCII)
        utf16le_to_utf8(hbuf, hlen, out, outsz);
    } else {
        // Already UTF-8
        size_t copy = (hlen < outsz - 1) ? hlen : outsz - 1;
        memcpy(out, hbuf, copy);
        out[copy] = '\0';
    }

    return (size_t)(4 + hlen);
}

// Find TWO distinct language matches, ordered by position of first occurrence
// in the text (left-to-right). This preserves title word order so that
// "Hindi-English Dictionary" → Hi / En, not En / Hi.
static void keyword_pair(const char *text, const char **l1, const char **l2) {
    *l1 = NULL; *l2 = NULL;

    char low[131072];
    strncpy(low, text, sizeof(low) - 1);
    low[sizeof(low)-1] = '\0';
    str_tolower(low);

    // For each keyword, find its leftmost position in the lowered text
    const char *best1_code = NULL, *best2_code = NULL;
    size_t best1_pos = (size_t)-1, best2_pos = (size_t)-1;

    for (int i = 0; kw_table[i].kw; i++) {
        char kw_low[64];
        strncpy(kw_low, kw_table[i].kw, sizeof(kw_low)-1);
        kw_low[sizeof(kw_low)-1] = '\0';
        str_tolower(kw_low);

        const char *hit = strstr(low, kw_low);
        if (!hit) continue;
        size_t pos = (size_t)(hit - low);
        const char *code = kw_table[i].code;

        if (best1_code == NULL || pos < best1_pos) {
            // New best first match — but don't displace if same code
            if (best1_code && strcmp(code, best1_code) == 0) {
                if (pos < best1_pos) best1_pos = pos;
                continue;
            }
            // Shift current best1 → candidate for best2
            if (best1_code && strcmp(code, best1_code) != 0) {
                if (best1_pos < best2_pos) {
                    best2_code = best1_code;
                    best2_pos  = best1_pos;
                }
            }
            best1_code = code;
            best1_pos  = pos;
        } else if (strcmp(code, best1_code) != 0) {
            if (best2_code == NULL || pos < best2_pos ||
                strcmp(code, best2_code) == 0) {
                if (best2_code == NULL || strcmp(code, best2_code) != 0 ||
                    pos < best2_pos) {
                    best2_code = code;
                    best2_pos  = pos;
                }
            }
        }
    }

    *l1 = best1_code;
    *l2 = best2_code;
}

// ─── script-based detection ───────────────────────────────────────────────
static const char* script_dominant(const ScriptCounts *sc,
                                   const unsigned char *buf, size_t n) {
    // CJK: split by kana/hangul
    if (sc->cjk_unified > SCRIPT_THRESH) {
        if (sc->hiragana + sc->katakana > SCRIPT_THRESH) return L_JA;
        if (sc->hangul > SCRIPT_THRESH)                  return L_KO;
        return L_ZH;
    }
    if (sc->hiragana + sc->katakana > SCRIPT_THRESH) return L_JA;
    if (sc->hangul > SCRIPT_THRESH)                  return L_KO;

    if (sc->arabic + sc->arabic_ext > SCRIPT_THRESH)
        return disambiguate_arabic_script(buf, n, sc);

    if (sc->devanagari > SCRIPT_THRESH)
        return disambiguate_devanagari(buf, n);

    if (sc->bengali   > SCRIPT_THRESH) return L_BN;
    if (sc->gurmukhi  > SCRIPT_THRESH) return L_PA;
    if (sc->gujarati  > SCRIPT_THRESH) return "Gu";
    if (sc->oriya     > SCRIPT_THRESH) return "Or";
    if (sc->tamil     > SCRIPT_THRESH) return L_TA;
    if (sc->telugu    > SCRIPT_THRESH) return L_TE;
    if (sc->kannada   > SCRIPT_THRESH) return L_KN;
    if (sc->malayalam > SCRIPT_THRESH) return L_ML;
    if (sc->sinhala   > SCRIPT_THRESH) return "Si";
    if (sc->thai      > SCRIPT_THRESH) return L_TH;
    if (sc->cyrillic  > SCRIPT_THRESH) return L_RU;  // refined below
    if (sc->greek     > SCRIPT_THRESH) return L_EL;
    if (sc->hebrew    > SCRIPT_THRESH) return L_HE;

    // dominant latin: check ratio
    if (sc->ascii_alpha > 50) return L_EN;

    return NULL;
}

// ─── Cyrillic disambiguation (Russian vs Ukrainian vs Bulgarian etc.) ─────
// Ukrainian has Ї (U+0407), Є (U+0404), І (U+0406)
static const char* disambiguate_cyrillic(const unsigned char *buf, size_t n) {
    int uk = 0;
    const unsigned char *p = buf, *end = buf + n;
    while (p < end) {
        uint32_t c = next_cp(&p, end);
        if (c == 0x0407 || c == 0x0404 || c == 0x0406) uk++;
    }
    return (uk > 3) ? L_UK : L_RU;
}

// ─── main ─────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    int verbose = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else path = argv[i];
    }

    if (!path) {
        fprintf(stderr, "Usage: %s [-v] file.mdx\n", argv[0]);
        fprintf(stderr, "  -v  verbose (show detection steps)\n");
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }

    size_t total = HEADER_READ + BODY_READ;
    unsigned char *buf = malloc(total + 1);
    if (!buf) { fputs("out of memory\n", stderr); return 1; }

    size_t n = fread(buf, 1, total, f);
    fclose(f);
    buf[n] = '\0';

    // ── Step 1: parse MDX header ──────────────────────────────────────────
    char header_text[131072];
    size_t body_off = extract_header(buf, n, header_text, sizeof(header_text));
    extract_attr_values(header_text);

    // ── Step 1b: extract filename basename (strip path + .mdx extension) ──
    // Many real-world MDX files have empty Title/Description but informative
    // filenames, e.g. "دستور العلماء.mdx" or "中华成语大词典.mdx".
    // Append the basename to header_text so keyword scan + census both see it.
    {
        // find last '/' or '\'
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;

        // copy basename, strip extension
        char fname[1024];
        strncpy(fname, base, sizeof(fname) - 1);
        fname[sizeof(fname)-1] = '\0';
        char *dot = strrchr(fname, '.');
        if (dot) *dot = '\0';

        // append to header_text with a space separator
        size_t hl = strlen(header_text);
        size_t fl = strlen(fname);
        if (hl + fl + 2 < sizeof(header_text)) {
            header_text[hl] = ' ';
            memcpy(header_text + hl + 1, fname, fl + 1);
        }
    }

    if (verbose) fprintf(stderr, "[header+fname] %.400s\n", header_text);

    // ── Step 2: keyword scan on header + filename ─────────────────────────
    const char *kw1 = NULL, *kw2 = NULL;
    keyword_pair(header_text, &kw1, &kw2);

    if (verbose)
        fprintf(stderr, "[keyword pair] %s / %s\n",
                kw1 ? kw1 : "-", kw2 ? kw2 : "-");

    // ── Step 3: Unicode script census ────────────────────────────────────
    // Sources (in reliability order):
    //   (a) decoded header_text + filename (UTF-8, non-ASCII preserved)
    //   (b) raw body bytes (useful if MDX has uncompressed early entries;
    //       compressed blocks are noise but still counted with low weight)
    ScriptCounts sc_hdr, sc_body;
    census((const unsigned char *)header_text, strlen(header_text), &sc_hdr);

    if (body_off > 0 && body_off < n)
        census(buf + body_off, n - body_off, &sc_body);
    else
        memset(&sc_body, 0, sizeof(sc_body));

    // Merge: header+fname weighted ×8, body ×1
    ScriptCounts sc;
#define MERGE(field) sc.field = sc_hdr.field * 8 + sc_body.field
    MERGE(cjk_unified); MERGE(hiragana); MERGE(katakana); MERGE(hangul);
    MERGE(arabic); MERGE(arabic_ext); MERGE(hebrew);
    MERGE(devanagari); MERGE(bengali); MERGE(gurmukhi); MERGE(gujarati);
    MERGE(oriya); MERGE(tamil); MERGE(telugu); MERGE(kannada);
    MERGE(malayalam); MERGE(sinhala); MERGE(thai);
    MERGE(cyrillic); MERGE(greek); MERGE(latin_ext); MERGE(ascii_alpha);
#undef MERGE

    if (verbose) {
        fprintf(stderr, "[census hdr+fname] cjk=%d hira=%d kata=%d hangul=%d "
                        "arabic=%d arabic_ext=%d deva=%d cyrillic=%d latin=%d\n",
                sc_hdr.cjk_unified, sc_hdr.hiragana, sc_hdr.katakana, sc_hdr.hangul,
                sc_hdr.arabic, sc_hdr.arabic_ext, sc_hdr.devanagari,
                sc_hdr.cyrillic, sc_hdr.ascii_alpha);
        fprintf(stderr, "[census body]      cjk=%d hira=%d kata=%d hangul=%d "
                        "arabic=%d arabic_ext=%d deva=%d cyrillic=%d latin=%d\n",
                sc_body.cjk_unified, sc_body.hiragana, sc_body.katakana, sc_body.hangul,
                sc_body.arabic, sc_body.arabic_ext, sc_body.devanagari,
                sc_body.cyrillic, sc_body.ascii_alpha);
    }

    const char *script1 = script_dominant(&sc, (const unsigned char *)header_text,
                                          strlen(header_text));

    // Refine Cyrillic
    if (script1 && strcmp(script1, L_RU) == 0)
        script1 = disambiguate_cyrillic((const unsigned char *)header_text,
                                        strlen(header_text));

    if (verbose)
        fprintf(stderr, "[script dominant] %s\n", script1 ? script1 : "-");

    // ── Step 4: combine sources ───────────────────────────────────────────
    const char *lang1 = NULL, *lang2 = NULL;

    // Keyword hit is most reliable for explicit language names
    if (kw1) {
        lang1 = kw1;
        lang2 = kw2;  // may be NULL
    }

    // Fill missing slots from script
    if (!lang1 && script1) lang1 = script1;

    // If keyword gave us one language and script gives a different one → bilingual
    if (lang1 && !lang2 && script1 && strcmp(lang1, script1) != 0)
        lang2 = script1;

    // ── Step 5: monolingual heuristic ─────────────────────────────────────
    // If only one language detected and header hints monolingual, lang2 = lang1
    if (lang1 && !lang2) {
        // Check if the dominant script matches the keyword lang
        // If they agree, probably monolingual
        if (!script1 || strcmp(lang1, script1) == 0)
            lang2 = lang1;
        else
            lang2 = script1;
    }

    // ── Step 6: fallback ──────────────────────────────────────────────────
    if (!lang1) lang1 = L_UNK;
    if (!lang2) lang2 = lang1;

    // Output in detection order — keyword_pair preserves left-to-right title order,
    // which matches dictionary convention (source language first).
    // Do NOT sort alphabetically: "Hindi-English" must stay Hi-En, not En-Hi.
    printf("%s-%s\n", lang1, lang2);

    free(buf);
    return 0;
}
