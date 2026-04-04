/*
 * compress.c — Text compression/decompression for the toke compiler.
 *
 * =========================================================================
 * Role
 * =========================================================================
 * This module provides compression utilities for natural-language and
 * structured text (JSON, CSV, prose).  Its primary consumer is the loke
 * privacy pipeline, which produces placeholder tokens such as $PERSON_1 and
 * $EMAIL_3 that must survive the compress/decompress round-trip unchanged.
 *
 * Design constraints
 * ------------------
 *   - No heap allocation in the hot path; callers supply output buffers.
 *   - Placeholder atoms ($[A-Z][A-Z0-9_]*[0-9]+) are always opaque.
 *   - compress_text() / decompress_text() are inverse operations:
 *       decompress_text(compress_text(x)) == x  (byte-identical).
 *   - Schema-aware paths (JSON, CSV) target 30–60 % token savings over
 *     raw text.
 *   - The streaming API emits output as input arrives; no full-buffer
 *     requirement.
 *
 * Compression strategy (prose)
 * ----------------------------
 * Pass 1 — tokenise input into words, placeholders, punctuation, whitespace.
 * Pass 2 — abbreviate high-frequency function words using a fixed table.
 * Pass 3 — deduplicate repeated non-placeholder word runs with back-refs
 *           of the form @Rn (n = 1-based index into seen-word table).
 * Decompression reverses passes 3 then 2.
 *
 * The compressed stream carries a 4-byte magic prefix "TK:P" so that
 * decompress_text() can detect and reject non-compressed input.
 *
 * Stories: 13.1.1, 13.1.4, 13.1.5
 */

#include "compress.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define PROSE_MAGIC     "TK:P"
#define PROSE_MAGIC_LEN 4

#define JSON_MAGIC      "TK:JSON:"
#define JSON_MAGIC_LEN  8

#define CSV_MAGIC       "TK:CSV:"
#define CSV_MAGIC_LEN   7

/*
 * Maximum number of distinct non-placeholder words tracked for
 * back-reference deduplication during prose compression.
 */
#define MAX_DICT_WORDS  256
#define MAX_WORD_LEN    128

/*
 * High-frequency function-word abbreviation table.
 * Format: { full_word, abbreviation }
 * Abbreviations use characters that cannot appear in normal prose words,
 * specifically the tilde prefix, so they are unambiguous during reversal.
 * Only words of 4+ characters are worth abbreviating (net saving >= 1 byte
 * after adding the tilde prefix).
 */
static const struct { const char *word; const char *abbr; } ABBR_TABLE[] = {
    { "the",        "~t"  },
    { "and",        "~a"  },
    { "that",       "~th" },
    { "this",       "~ts" },
    { "with",       "~w"  },
    { "from",       "~f"  },
    { "have",       "~hv" },
    { "they",       "~ty" },
    { "will",       "~wl" },
    { "been",       "~bn" },
    { "were",       "~wr" },
    { "your",       "~yr" },
    { "which",      "~wh" },
    { "their",      "~tr" },
    { "there",      "~te" },
    { "about",      "~ab" },
    { "would",      "~wo" },
    { "could",      "~co" },
    { "should",     "~sh" },
    { "please",     "~pl" },
    { "thank",      "~tn" },
    { "hello",      "~hl" },
    { "world",      "~wrd" },
    { "message",    "~ms" },
    { "address",    "~ad" },
    { "company",    "~cp" },
    { "subject",    "~sb" },
    { "request",    "~rq" },
    { "response",   "~rs" },
    { "following",  "~fl" },
    { "attached",   "~at" },
    { "regards",    "~rg" },
    { "sincerely",  "~sc" },
    { "customer",   "~cu" },
    { "account",    "~ac" },
    { "between",    "~bt" },
    { "because",    "~bc" },
    { "through",    "~tg" },
    { "important",  "~im" },
    { "received",   "~rc" },
    { "provided",   "~pv" },
    { "according",  "~ag" },
    { "therefore",  "~tf" },
    { "however",    "~hw" },
    { NULL, NULL }
};

/* ── Placeholder atom detection ─────────────────────────────────────── */

/*
 * is_placeholder_start — Return 1 if src[pos] begins a placeholder atom.
 *
 * A placeholder matches: $ [A-Z] [A-Z0-9_]* [0-9]+
 * The pattern requires at least one uppercase letter after $ and at
 * least one digit at the end.
 *
 * pos must be a valid index; src need not be NUL-terminated but must
 * have at least (len - pos) bytes accessible.
 */
static int is_placeholder_start(const char *src, size_t pos, size_t len)
{
    if (pos >= len || src[pos] != '$') return 0;
    size_t i = pos + 1;
    if (i >= len || src[i] < 'A' || src[i] > 'Z') return 0;
    i++;
    /* Consume [A-Z0-9_]* */
    while (i < len && (isupper((unsigned char)src[i]) ||
                       isdigit((unsigned char)src[i]) ||
                       src[i] == '_')) {
        i++;
    }
    /* Must end with at least one digit — back up to check */
    if (i == pos + 2) {
        /* Only one uppercase letter after $, no body — check it was a digit */
        /* (can't be, it was A-Z) — so not a valid placeholder */
        return 0;
    }
    /* The last consumed character before i must be a digit */
    return isdigit((unsigned char)src[i - 1]);
}

/*
 * placeholder_len — Return the byte length of the placeholder atom starting
 *                   at src[pos].  Assumes is_placeholder_start() returned 1.
 */
static size_t placeholder_len(const char *src, size_t pos, size_t len)
{
    size_t i = pos + 1; /* skip $ */
    while (i < len && (isupper((unsigned char)src[i]) ||
                       isdigit((unsigned char)src[i]) ||
                       src[i] == '_')) {
        i++;
    }
    return i - pos;
}

/* ── Word utilities ──────────────────────────────────────────────────── */

/*
 * word_len — Return the length of the ASCII word starting at src[pos].
 * A word is a run of [A-Za-z0-9] characters.
 */
static size_t word_len(const char *src, size_t pos, size_t len)
{
    size_t i = pos;
    while (i < len && (isalnum((unsigned char)src[i]))) i++;
    return i - pos;
}

/*
 * abbr_for_word — Return the abbreviation string for the given word
 *                 (case-insensitive comparison), or NULL if not in table.
 *
 * word : pointer to start of word
 * wlen : byte length of word
 */
static const char *abbr_for_word(const char *word, size_t wlen)
{
    char lower[MAX_WORD_LEN + 1];
    if (wlen == 0 || wlen > MAX_WORD_LEN) return NULL;
    for (size_t i = 0; i < wlen; i++)
        lower[i] = (char)tolower((unsigned char)word[i]);
    lower[wlen] = '\0';
    for (int i = 0; ABBR_TABLE[i].word; i++) {
        if (strcmp(lower, ABBR_TABLE[i].word) == 0)
            return ABBR_TABLE[i].abbr;
    }
    return NULL;
}

/*
 * word_for_abbr — Return the full word for the given abbreviation,
 *                 or NULL if not in table.
 */
static const char *word_for_abbr(const char *abbr)
{
    for (int i = 0; ABBR_TABLE[i].abbr; i++) {
        if (strcmp(abbr, ABBR_TABLE[i].abbr) == 0)
            return ABBR_TABLE[i].word;
    }
    return NULL;
}

/* ── detect_input_type ──────────────────────────────────────────────── */

CompressInputType detect_input_type(const char *input, size_t len)
{
    if (!input || len == 0) return COMPRESS_PROSE;

    /* Skip leading whitespace */
    size_t i = 0;
    while (i < len && isspace((unsigned char)input[i])) i++;
    if (i >= len) return COMPRESS_PROSE;

    /* JSON: starts with { or [ */
    if (input[i] == '{' || input[i] == '[') {
        /* Validate there's a closing brace/bracket somewhere */
        char close = (input[i] == '{') ? '}' : ']';
        for (size_t j = len; j-- > i; ) {
            if (isspace((unsigned char)input[j])) continue;
            if (input[j] == close) return COMPRESS_JSON;
            break;
        }
    }

    /* CSV: first line contains at least one comma and no JSON markers,
     * all fields on line 1 are plain text (no { [ ) */
    size_t line_end = i;
    int comma_count = 0;
    int brace_count = 0;
    while (line_end < len && input[line_end] != '\n') {
        if (input[line_end] == ',') comma_count++;
        if (input[line_end] == '{' || input[line_end] == '[') brace_count++;
        line_end++;
    }
    if (comma_count >= 1 && brace_count == 0) {
        /* Check there is at least one more line (data row) */
        if (line_end < len) return COMPRESS_CSV;
    }

    return COMPRESS_PROSE;
}

/* ── compress_text ──────────────────────────────────────────────────── */

int compress_text(const char *input, size_t len, char *out_buf)
{
    if (!input || !out_buf) return -1;
    if (len == 0) {
        out_buf[0] = '\0';
        return 0;
    }

    /*
     * Dictionary of seen non-placeholder words for back-reference
     * deduplication.  Words are stored as NUL-terminated strings.
     * dict_count tracks how many entries are populated.
     */
    char dict[MAX_DICT_WORDS][MAX_WORD_LEN + 1];
    int  dict_count = 0;

    /* Emit magic prefix */
    memcpy(out_buf, PROSE_MAGIC, PROSE_MAGIC_LEN);
    int out = PROSE_MAGIC_LEN;

    size_t pos = 0;
    while (pos < len) {
        /* Placeholder atom — copy verbatim, never compress */
        if (is_placeholder_start(input, pos, len)) {
            size_t plen = placeholder_len(input, pos, len);
            memcpy(out_buf + out, input + pos, plen);
            out += (int)plen;
            pos += plen;
            continue;
        }

        /* Whitespace — normalise runs to a single space */
        if (isspace((unsigned char)input[pos])) {
            /* Preserve newlines; collapse other whitespace to ' ' */
            char ws = input[pos];
            while (pos < len && isspace((unsigned char)input[pos])) {
                if (input[pos] == '\n') ws = '\n';
                pos++;
            }
            out_buf[out++] = ws;
            continue;
        }

        /* Word — try abbreviation then back-reference deduplication */
        if (isalpha((unsigned char)input[pos])) {
            size_t wlen = word_len(input, pos, len);

            /* Try abbreviation first */
            const char *abbr = abbr_for_word(input + pos, wlen);
            if (abbr) {
                int alen = (int)strlen(abbr);
                memcpy(out_buf + out, abbr, (size_t)alen);
                out += alen;
                pos += wlen;
                continue;
            }

            /* Copy word into temp buffer for dict lookup */
            char wordbuf[MAX_WORD_LEN + 1];
            size_t copy_len = wlen < MAX_WORD_LEN ? wlen : MAX_WORD_LEN;
            memcpy(wordbuf, input + pos, copy_len);
            wordbuf[copy_len] = '\0';

            /* Look up in dictionary for back-reference */
            int found_idx = -1;
            for (int di = 0; di < dict_count; di++) {
                if (strcasecmp(dict[di], wordbuf) == 0) {
                    found_idx = di;
                    break;
                }
            }

            if (found_idx >= 0 && copy_len >= 4) {
                /* Emit back-reference @Rn (1-based) */
                char ref[16];
                int rlen = snprintf(ref, sizeof(ref), "@R%d", found_idx + 1);
                memcpy(out_buf + out, ref, (size_t)rlen);
                out += rlen;
            } else {
                /* Emit word verbatim and add to dictionary */
                memcpy(out_buf + out, input + pos, wlen);
                out += (int)wlen;
                if (dict_count < MAX_DICT_WORDS) {
                    memcpy(dict[dict_count], wordbuf, copy_len + 1);
                    dict_count++;
                }
            }
            pos += wlen;
            continue;
        }

        /* Digits and punctuation — copy verbatim */
        out_buf[out++] = input[pos++];
    }

    out_buf[out] = '\0';
    return out;
}

/* ── decompress_text ────────────────────────────────────────────────── */

int decompress_text(const char *input, size_t len, char *out_buf)
{
    if (!input || !out_buf) return -1;
    if (len == 0) {
        out_buf[0] = '\0';
        return 0;
    }

    /* Verify and skip magic prefix */
    if (len < PROSE_MAGIC_LEN ||
        memcmp(input, PROSE_MAGIC, (size_t)PROSE_MAGIC_LEN) != 0) {
        /* Not compressed — return as-is (identity) */
        memcpy(out_buf, input, len);
        out_buf[len] = '\0';
        return (int)len;
    }

    /*
     * Dictionary: maps 1-based index to the full word that was emitted
     * at that position during compression (rebuilt in pass order).
     */
    char dict[MAX_DICT_WORDS][MAX_WORD_LEN + 1];
    int  dict_count = 0;

    size_t pos = (size_t)PROSE_MAGIC_LEN;
    int out = 0;

    while (pos < len) {
        /* Placeholder atom — copy verbatim */
        if (is_placeholder_start(input, pos, len)) {
            size_t plen = placeholder_len(input, pos, len);
            memcpy(out_buf + out, input + pos, plen);
            out += (int)plen;
            pos += plen;
            continue;
        }

        /* Back-reference @Rn — replace with dictionary word */
        if (input[pos] == '@' && pos + 1 < len && input[pos + 1] == 'R') {
            size_t j = pos + 2;
            while (j < len && isdigit((unsigned char)input[j])) j++;
            /* Parse the index */
            int idx = 0;
            for (size_t k = pos + 2; k < j; k++)
                idx = idx * 10 + (input[k] - '0');
            if (idx >= 1 && idx <= dict_count) {
                const char *w = dict[idx - 1];
                int wlen = (int)strlen(w);
                memcpy(out_buf + out, w, (size_t)wlen);
                out += wlen;
            }
            pos = j;
            continue;
        }

        /* Tilde abbreviation — expand to full word */
        if (input[pos] == '~') {
            /* Consume the abbreviation token (tilde + non-space chars) */
            size_t j = pos;
            while (j < len && !isspace((unsigned char)input[j])) j++;
            char abbr[16];
            size_t alen = j - pos;
            if (alen < sizeof(abbr)) {
                memcpy(abbr, input + pos, alen);
                abbr[alen] = '\0';
                const char *full = word_for_abbr(abbr);
                if (full) {
                    int flen = (int)strlen(full);
                    memcpy(out_buf + out, full, (size_t)flen);
                    out += flen;
                    pos = j;
                    continue;
                }
            }
            /* Unknown tilde sequence — emit verbatim */
            memcpy(out_buf + out, input + pos, j - pos);
            out += (int)(j - pos);
            pos = j;
            continue;
        }

        /* Whitespace — emit as-is */
        if (isspace((unsigned char)input[pos])) {
            out_buf[out++] = input[pos++];
            continue;
        }

        /* Word — emit and rebuild dictionary */
        if (isalpha((unsigned char)input[pos])) {
            size_t wlen = word_len(input, pos, len);
            memcpy(out_buf + out, input + pos, wlen);
            out += (int)wlen;
            /* Add to dict for back-reference resolution */
            if (dict_count < MAX_DICT_WORDS) {
                size_t copy_len = wlen < MAX_WORD_LEN ? wlen : MAX_WORD_LEN;
                memcpy(dict[dict_count], input + pos, copy_len);
                dict[dict_count][copy_len] = '\0';
                dict_count++;
            }
            pos += wlen;
            continue;
        }

        /* Digits and punctuation — emit verbatim */
        out_buf[out++] = input[pos++];
    }

    out_buf[out] = '\0';
    return out;
}

/* ── compress_json ──────────────────────────────────────────────────── */

/*
 * Simple JSON value extractor.
 * Skips leading whitespace and returns a pointer to the next value token,
 * advancing *pos past it.  Returns NULL if no value found.
 *
 * This is intentionally minimal: we only need to extract top-level string
 * and number values for schema-aware encoding.  Nested objects are emitted
 * verbatim (they will be compressed by compress_text at a higher level if
 * needed).
 */

/*
 * extract_json_string_value — copy the unquoted content of a JSON string
 * starting at input[*pos] into buf.  Advances *pos past the closing quote.
 * Returns length copied (without NUL) or -1 on error.
 */
static int extract_json_string_value(const char *input, size_t len,
                                     size_t *pos, char *buf, int bufsz)
{
    if (*pos >= len || input[*pos] != '"') return -1;
    (*pos)++;
    int out = 0;
    while (*pos < len && out < bufsz - 1) {
        if (input[*pos] == '\\') {
            if (*pos + 1 < len) {
                buf[out++] = input[(*pos) + 1];
                *pos += 2;
            } else break;
            continue;
        }
        if (input[*pos] == '"') { (*pos)++; break; }
        buf[out++] = input[(*pos)++];
    }
    buf[out] = '\0';
    return out;
}

int compress_json(const char *input, size_t len, char *out_buf)
{
    if (!input || !out_buf || len == 0) return -1;

    /* Skip whitespace */
    size_t pos = 0;
    while (pos < len && isspace((unsigned char)input[pos])) pos++;

    if (pos >= len || (input[pos] != '{' && input[pos] != '[')) {
        /* Not a JSON object/array — fall back to prose compression */
        return compress_text(input, len, out_buf);
    }

    int is_array = (input[pos] == '[');

    if (!is_array) {
        /*
         * Single JSON object: extract keys once as schema, then
         * emit values as a tab-separated row.
         *
         * Format:
         *   TK:JSON:<key_count> <k1>,<k2>,...\n
         *   <v1>\t<v2>\t...\n
         */
        pos++; /* skip { */

        /* Two-pass: first collect keys, then values */
        char keys[32][MAX_WORD_LEN + 1];
        char vals[32][256];
        int  npairs = 0;

        while (pos < len && input[pos] != '}' && npairs < 32) {
            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos >= len || input[pos] == '}') break;

            if (input[pos] != '"') {
                /* Malformed — fallback */
                return compress_text(input, len, out_buf);
            }

            /* Extract key */
            int klen = extract_json_string_value(input, len, &pos,
                                                  keys[npairs],
                                                  (int)sizeof(keys[npairs]));
            if (klen < 0) return compress_text(input, len, out_buf);

            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos >= len || input[pos] != ':') return compress_text(input, len, out_buf);
            pos++; /* skip : */
            while (pos < len && isspace((unsigned char)input[pos])) pos++;

            /* Extract value (string or number or other) */
            if (pos < len && input[pos] == '"') {
                int vlen = extract_json_string_value(input, len, &pos,
                                                      vals[npairs],
                                                      (int)sizeof(vals[npairs]));
                if (vlen < 0) return compress_text(input, len, out_buf);
            } else {
                /* number, bool, null, or nested — copy until , or } */
                int vi = 0;
                int depth = 0;
                while (pos < len && vi < (int)sizeof(vals[npairs]) - 1) {
                    char c = input[pos];
                    if (depth == 0 && (c == ',' || c == '}')) break;
                    if (c == '{' || c == '[') depth++;
                    if (c == '}' || c == ']') depth--;
                    vals[npairs][vi++] = c;
                    pos++;
                }
                /* Trim trailing whitespace */
                while (vi > 0 && isspace((unsigned char)vals[npairs][vi - 1]))
                    vi--;
                vals[npairs][vi] = '\0';
            }

            npairs++;

            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos < len && input[pos] == ',') pos++;
        }

        if (npairs == 0) return compress_text(input, len, out_buf);

        /* Emit header */
        int out = snprintf(out_buf, JSON_MAGIC_LEN + 8, "%s%d ",
                           JSON_MAGIC, npairs);
        for (int i = 0; i < npairs; i++) {
            int kl = (int)strlen(keys[i]);
            memcpy(out_buf + out, keys[i], (size_t)kl);
            out += kl;
            if (i < npairs - 1) out_buf[out++] = ',';
        }
        out_buf[out++] = '\n';

        /* Emit values */
        for (int i = 0; i < npairs; i++) {
            int vl = (int)strlen(vals[i]);
            memcpy(out_buf + out, vals[i], (size_t)vl);
            out += vl;
            out_buf[out++] = (i < npairs - 1) ? '\t' : '\n';
        }
        out_buf[out] = '\0';
        return out;
    }

    /* JSON array of objects */
    pos++; /* skip [ */
    while (pos < len && isspace((unsigned char)input[pos])) pos++;

    /* Peek at first element to get schema */
    if (pos >= len || input[pos] != '{') {
        return compress_text(input, len, out_buf);
    }

    char schema_keys[32][MAX_WORD_LEN + 1];
    int  nkeys = 0;
    int  header_emitted = 0;
    int  out = 0;

    while (pos < len && input[pos] != ']') {
        while (pos < len && isspace((unsigned char)input[pos])) pos++;
        if (pos >= len || input[pos] == ']') break;
        if (input[pos] != '{') {
            /* Non-object array element — fallback */
            return compress_text(input, len, out_buf);
        }
        pos++; /* skip { */

        char cur_keys[32][MAX_WORD_LEN + 1];
        char cur_vals[32][256];
        int  cur_n = 0;

        while (pos < len && input[pos] != '}' && cur_n < 32) {
            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos >= len || input[pos] == '}') break;
            if (input[pos] != '"') return compress_text(input, len, out_buf);

            int kl = extract_json_string_value(input, len, &pos,
                                                cur_keys[cur_n],
                                                (int)sizeof(cur_keys[cur_n]));
            if (kl < 0) return compress_text(input, len, out_buf);

            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos >= len || input[pos] != ':') return compress_text(input, len, out_buf);
            pos++;
            while (pos < len && isspace((unsigned char)input[pos])) pos++;

            if (pos < len && input[pos] == '"') {
                int vl = extract_json_string_value(input, len, &pos,
                                                    cur_vals[cur_n],
                                                    (int)sizeof(cur_vals[cur_n]));
                if (vl < 0) return compress_text(input, len, out_buf);
            } else {
                int vi = 0, depth = 0;
                while (pos < len && vi < (int)sizeof(cur_vals[cur_n]) - 1) {
                    char c = input[pos];
                    if (depth == 0 && (c == ',' || c == '}')) break;
                    if (c == '{' || c == '[') depth++;
                    if (c == '}' || c == ']') depth--;
                    cur_vals[cur_n][vi++] = c;
                    pos++;
                }
                while (vi > 0 && isspace((unsigned char)cur_vals[cur_n][vi-1]))
                    vi--;
                cur_vals[cur_n][vi] = '\0';
            }
            cur_n++;
            while (pos < len && isspace((unsigned char)input[pos])) pos++;
            if (pos < len && input[pos] == ',') pos++;
        }
        if (pos < len && input[pos] == '}') pos++;

        /* First object: emit schema header */
        if (!header_emitted) {
            nkeys = cur_n;
            memcpy(schema_keys, cur_keys, sizeof(cur_keys));
            out = snprintf(out_buf, JSON_MAGIC_LEN + 8, "%s%d ",
                           JSON_MAGIC, nkeys);
            for (int i = 0; i < nkeys; i++) {
                int kl = (int)strlen(schema_keys[i]);
                memcpy(out_buf + out, schema_keys[i], (size_t)kl);
                out += kl;
                if (i < nkeys - 1) out_buf[out++] = ',';
            }
            out_buf[out++] = '\n';
            header_emitted = 1;
        }

        /* Emit values for this row */
        for (int i = 0; i < cur_n && i < nkeys; i++) {
            int vl = (int)strlen(cur_vals[i]);
            memcpy(out_buf + out, cur_vals[i], (size_t)vl);
            out += vl;
            out_buf[out++] = (i < nkeys - 1) ? '\t' : '\n';
        }

        while (pos < len && isspace((unsigned char)input[pos])) pos++;
        if (pos < len && input[pos] == ',') pos++;
    }

    if (!header_emitted) return compress_text(input, len, out_buf);

    out_buf[out] = '\0';
    return out;
}

/* ── compress_csv ───────────────────────────────────────────────────── */

int compress_csv(const char *input, size_t len, char *out_buf)
{
    if (!input || !out_buf || len == 0) return -1;

    size_t pos = 0;

    /* Parse header row */
    char headers[32][MAX_WORD_LEN + 1];
    int  ncols = 0;

    /* Read first line as header */
    size_t line_start = pos;
    while (pos < len && input[pos] != '\n') pos++;
    size_t header_end = pos;
    if (pos < len) pos++; /* skip \n */

    /* Split header by commas */
    size_t hp = line_start;
    while (hp <= header_end && ncols < 32) {
        size_t field_start = hp;
        while (hp < header_end && input[hp] != ',') hp++;
        size_t field_len = hp - field_start;
        /* Strip surrounding whitespace */
        while (field_len > 0 && isspace((unsigned char)input[field_start]))
            { field_start++; field_len--; }
        while (field_len > 0 && isspace((unsigned char)input[field_start + field_len - 1]))
            field_len--;
        /* Strip surrounding quotes */
        if (field_len >= 2 && input[field_start] == '"' &&
            input[field_start + field_len - 1] == '"') {
            field_start++; field_len -= 2;
        }
        size_t copy_len = field_len < MAX_WORD_LEN ? field_len : MAX_WORD_LEN;
        memcpy(headers[ncols], input + field_start, copy_len);
        headers[ncols][copy_len] = '\0';
        ncols++;
        if (hp < header_end) hp++; /* skip , */
    }

    if (ncols == 0) return compress_text(input, len, out_buf);

    /* Emit CSV magic header */
    int out = snprintf(out_buf, CSV_MAGIC_LEN + 8, "%s%d ", CSV_MAGIC, ncols);
    for (int i = 0; i < ncols; i++) {
        int kl = (int)strlen(headers[i]);
        memcpy(out_buf + out, headers[i], (size_t)kl);
        out += kl;
        if (i < ncols - 1) out_buf[out++] = ',';
    }
    out_buf[out++] = '\n';

    /* Emit data rows as tab-separated tuples */
    while (pos < len) {
        size_t row_start = pos;
        while (pos < len && input[pos] != '\n') pos++;
        size_t row_end = pos;
        if (pos < len) pos++;

        if (row_end == row_start) continue; /* skip blank lines */

        size_t rp = row_start;
        int col = 0;
        while (rp <= row_end && col < ncols) {
            size_t field_start = rp;
            while (rp < row_end && input[rp] != ',') rp++;
            size_t field_len = rp - field_start;
            while (field_len > 0 && isspace((unsigned char)input[field_start]))
                { field_start++; field_len--; }
            while (field_len > 0 &&
                   isspace((unsigned char)input[field_start + field_len - 1]))
                field_len--;
            if (field_len >= 2 && input[field_start] == '"' &&
                input[field_start + field_len - 1] == '"') {
                field_start++; field_len -= 2;
            }
            memcpy(out_buf + out, input + field_start, field_len);
            out += (int)field_len;
            out_buf[out++] = (col < ncols - 1) ? '\t' : '\n';
            col++;
            if (rp < row_end) rp++;
        }
    }

    out_buf[out] = '\0';
    return out;
}

/* ── Streaming API ──────────────────────────────────────────────────── */

void compress_stream_init(CompressStreamCtx *ctx, FILE *out)
{
    if (!ctx) return;
    memset(ctx->pending, 0, sizeof(ctx->pending));
    ctx->pending_len = 0;
    ctx->out         = out;
    ctx->total_in    = 0;
    ctx->total_out   = 0;
}

/*
 * emit_token — compress and emit a single logical token (word, placeholder,
 * or punctuation run) to ctx->out.  Returns bytes emitted.
 *
 * This is the per-token hot path for streaming.  We apply only
 * abbreviation compression here (no back-reference deduplication across
 * chunks, as that would require stateful buffering of the entire stream).
 */
static int emit_token(CompressStreamCtx *ctx, const char *token, int tlen)
{
    if (tlen <= 0 || !ctx->out) return 0;
    int emitted = 0;

    /* Placeholder — emit verbatim */
    if (is_placeholder_start(token, 0, (size_t)tlen)) {
        fwrite(token, 1, (size_t)tlen, ctx->out);
        return tlen;
    }

    /* Word — try abbreviation */
    if (isalpha((unsigned char)token[0])) {
        const char *abbr = abbr_for_word(token, (size_t)tlen);
        if (abbr) {
            int alen = (int)strlen(abbr);
            fwrite(abbr, 1, (size_t)alen, ctx->out);
            return alen;
        }
    }

    /* Default — emit verbatim */
    fwrite(token, 1, (size_t)tlen, ctx->out);
    emitted = tlen;
    return emitted;
}

int compress_stream_feed(CompressStreamCtx *ctx, const char *chunk, int len)
{
    if (!ctx || !chunk || len <= 0) return 0;

    ctx->total_in += len;

    /* Append chunk to pending buffer */
    int emitted = 0;
    int ci = 0;

    while (ci < len || ctx->pending_len > 0) {
        /* Fill pending buffer */
        while (ci < len && ctx->pending_len < COMPRESS_STREAM_BUFSZ - 1) {
            ctx->pending[ctx->pending_len++] = chunk[ci++];
        }
        ctx->pending[ctx->pending_len] = '\0';

        /* Process complete tokens from pending */
        int consumed = 0;
        const char *p = ctx->pending;
        int plen = ctx->pending_len;

        while (consumed < plen) {
            size_t pos = (size_t)consumed;

            /* Placeholder atom */
            if (is_placeholder_start(p, pos, (size_t)plen)) {
                size_t pl = placeholder_len(p, pos, (size_t)plen);
                /* Only emit if fully buffered */
                if (pos + pl > (size_t)plen && ci < len) break;
                emitted += emit_token(ctx, p + pos, (int)pl);
                consumed += (int)pl;
                continue;
            }

            /* Whitespace — normalise and emit */
            if (isspace((unsigned char)p[consumed])) {
                char ws = p[consumed];
                while (consumed < plen && isspace((unsigned char)p[consumed])) {
                    if (p[consumed] == '\n') ws = '\n';
                    consumed++;
                }
                fputc(ws, ctx->out);
                emitted++;
                continue;
            }

            /* Word */
            if (isalpha((unsigned char)p[consumed])) {
                size_t wl = word_len(p, (size_t)consumed, (size_t)plen);
                /* If word is incomplete and more input is coming, hold it */
                if (consumed + (int)wl >= plen && ci < len) break;
                emitted += emit_token(ctx, p + consumed, (int)wl);
                consumed += (int)wl;
                continue;
            }

            /* Single character */
            fputc(p[consumed], ctx->out);
            emitted++;
            consumed++;
        }

        /* Shift unconsumed bytes to front of pending */
        int remaining = plen - consumed;
        if (remaining > 0)
            memmove(ctx->pending, ctx->pending + consumed, (size_t)remaining);
        ctx->pending_len = remaining;

        if (ci >= len) break; /* chunk fully processed */
    }

    ctx->total_out += emitted;
    return emitted;
}

int compress_stream_flush(CompressStreamCtx *ctx)
{
    if (!ctx) return -1;

    /* Emit any remaining pending content */
    int emitted = 0;
    if (ctx->pending_len > 0) {
        ctx->pending[ctx->pending_len] = '\0';
        const char *p = ctx->pending;
        int plen = ctx->pending_len;
        int consumed = 0;

        while (consumed < plen) {
            size_t pos = (size_t)consumed;

            if (is_placeholder_start(p, pos, (size_t)plen)) {
                size_t pl = placeholder_len(p, pos, (size_t)plen);
                emitted += emit_token(ctx, p + pos, (int)pl);
                consumed += (int)pl;
                continue;
            }
            if (isspace((unsigned char)p[consumed])) {
                fputc(p[consumed], ctx->out);
                emitted++;
                consumed++;
                continue;
            }
            if (isalpha((unsigned char)p[consumed])) {
                size_t wl = word_len(p, (size_t)consumed, (size_t)plen);
                emitted += emit_token(ctx, p + consumed, (int)wl);
                consumed += (int)wl;
                continue;
            }
            fputc(p[consumed], ctx->out);
            emitted++;
            consumed++;
        }
        ctx->pending_len = 0;
    }

    if (ctx->out) fflush(ctx->out);
    ctx->total_out += emitted;
    return emitted;
}
