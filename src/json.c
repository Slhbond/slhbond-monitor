/*
 * json.c - JSON member writer and a tolerant flat-object reader.
 */
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

static void escape_into(strbuf_t *sb, const char *s, size_t len)
{
    size_t i;

    sb_appendc(sb, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\b': sb_append(sb, "\\b");  break;
        case '\f': sb_append(sb, "\\f");  break;
        case '\n': sb_append(sb, "\\n");  break;
        case '\r': sb_append(sb, "\\r");  break;
        case '\t': sb_append(sb, "\\t");  break;
        default:
            if (c < 0x20)
                sb_appendf(sb, "\\u%04x", c);
            else
                sb_appendc(sb, (char)c);
            break;
        }
    }
    sb_appendc(sb, '"');
}

void json_escape(strbuf_t *sb, const char *s)
{
    escape_into(sb, s ? s : "", s ? strlen(s) : 0);
}

void json_key(strbuf_t *sb, const char *key, int *first)
{
    if (first && *first)
        *first = 0;
    else
        sb_appendc(sb, ',');
    json_escape(sb, key);
    sb_appendc(sb, ':');
}

static void member_prefix(strbuf_t *sb, const char *key, int *first)
{
    json_key(sb, key, first);
}

void json_add_str(strbuf_t *sb, const char *key, const char *val, int *first)
{
    member_prefix(sb, key, first);
    if (val)
        json_escape(sb, val);
    else
        sb_append(sb, "null");
}

void json_add_int(strbuf_t *sb, const char *key, long long val, int *first)
{
    member_prefix(sb, key, first);
    sb_appendf(sb, "%lld", val);
}

void json_add_uint(strbuf_t *sb, const char *key, unsigned long long val, int *first)
{
    member_prefix(sb, key, first);
    sb_appendf(sb, "%llu", val);
}

void json_add_bool(strbuf_t *sb, const char *key, int val, int *first)
{
    member_prefix(sb, key, first);
    sb_append(sb, val ? "true" : "false");
}

void json_add_double(strbuf_t *sb, const char *key, double val, int *first)
{
    member_prefix(sb, key, first);
    sb_appendf(sb, "%.2f", val);
}

void json_add_null(strbuf_t *sb, const char *key, int *first)
{
    member_prefix(sb, key, first);
    sb_append(sb, "null");
}

void json_add_raw(strbuf_t *sb, const char *key, const char *raw, int *first)
{
    member_prefix(sb, key, first);
    sb_append(sb, raw ? raw : "null");
}

void json_add_str_array(strbuf_t *sb, const char *key,
                        const char *const *items, size_t count, int *first)
{
    size_t i;

    member_prefix(sb, key, first);
    sb_appendc(sb, '[');
    for (i = 0; i < count; i++) {
        if (i)
            sb_appendc(sb, ',');
        json_escape(sb, items[i]);
    }
    sb_appendc(sb, ']');
}

/* ------------------------------------------------------------------ */
/* Reader: scanner primitives                                          */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/** `p` points at the opening quote; returns the position just past the closing one. */
static const char *skip_string(const char *p)
{
    if (*p != '"')
        return p;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        if (*p == '"')
            return p + 1;
        p++;
    }
    return p;
}

/** `p` points at '{' or '['; returns the position just past its match. */
static const char *skip_container(const char *p)
{
    int depth = 0;

    while (*p) {
        if (*p == '"') {
            p = skip_string(p);
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}' || *p == ']') {
            depth--;
            p++;
            if (depth <= 0)
                return p;
            continue;
        }
        p++;
    }
    return p;
}

/** Returns the position just past the value starting at `p`. */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '{' || *p == '[')
        return skip_container(p);
    if (*p == '"')
        return skip_string(p);
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
        p++;
    return p;
}

/* ------------------------------------------------------------------ */
/* Reader: value decoding                                              */
/* ------------------------------------------------------------------ */

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int c = (unsigned char)p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static void utf8_put(strbuf_t *sb, unsigned cp)
{
    if (cp < 0x80) {
        sb_appendc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_appendc(sb, (char)(0xC0 | (cp >> 6)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_appendc(sb, (char)(0xE0 | (cp >> 12)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_appendc(sb, (char)(0xF0 | (cp >> 18)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/**
 * Decode the JSON string starting at `p` (which must point at '"') into `sb`.
 * Returns the position just past the closing quote, or NULL on malformed input.
 */
static const char *decode_string(const char *p, strbuf_t *sb)
{
    if (*p != '"')
        return NULL;
    p++;
    while (*p) {
        if (*p == '"')
            return p + 1;
        if (*p != '\\') {
            sb_appendc(sb, *p++);
            continue;
        }
        p++;
        switch (*p) {
        case '"':  sb_appendc(sb, '"');  p++; break;
        case '\\': sb_appendc(sb, '\\'); p++; break;
        case '/':  sb_appendc(sb, '/');  p++; break;
        case 'b':  sb_appendc(sb, '\b'); p++; break;
        case 'f':  sb_appendc(sb, '\f'); p++; break;
        case 'n':  sb_appendc(sb, '\n'); p++; break;
        case 'r':  sb_appendc(sb, '\r'); p++; break;
        case 't':  sb_appendc(sb, '\t'); p++; break;
        case 'u': {
            unsigned cp;
            if (hex4(p + 1, &cp) != 0)
                return NULL;
            p += 5;
            /* Combine a UTF-16 surrogate pair when present. */
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                unsigned lo;
                if (hex4(p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_put(sb, cp);
            break;
        }
        default:
            return NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Reader: top-level lookup                                            */
/* ------------------------------------------------------------------ */

/**
 * Walk the top-level members of the object in `json`, invoking `visit` for each
 * "key": value pair. Iteration stops when `visit` returns non-zero.
 */
typedef int (*member_visitor_fn)(const char *key, const char *value, void *ctx);

static int json_flat_visit(const char *json, member_visitor_fn visit, void *ctx)
{
    const char *p = skip_ws(json);
    int found = 0;

    if (*p != '{')
        return 0;
    p = skip_ws(p + 1);
    if (*p == '}')
        return 0;

    for (;;) {
        strbuf_t key;
        const char *value;

        p = skip_ws(p);
        if (*p != '"')
            break;
        sb_init(&key);
        p = decode_string(p, &key);
        if (!p) {
            sb_free(&key);
            break;
        }
        p = skip_ws(p);
        if (*p != ':') {
            sb_free(&key);
            break;
        }
        value = skip_ws(p + 1);

        if (visit(key.data ? key.data : "", value, ctx)) {
            found = 1;
            sb_free(&key);
            break;
        }
        sb_free(&key);

        p = skip_ws(skip_value(value));
        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }
    return found;
}

typedef struct {
    const char *want;
    strbuf_t   *out;
    int         found;
} str_lookup_t;

static int visit_str(const char *key, const char *value, void *ctx)
{
    str_lookup_t *lk = ctx;

    if (strcmp(key, lk->want) != 0 || *value != '"')
        return 0;
    if (!decode_string(value, lk->out))
        return 0;
    lk->found = 1;
    return 1;
}

int json_flat_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    str_lookup_t lk;
    strbuf_t buf;
    int rc;

    if (!json || !key || !out || out_size == 0)
        return 0;
    out[0] = '\0';
    sb_init(&buf);
    lk.want = key;
    lk.out = &buf;
    lk.found = 0;
    rc = json_flat_visit(json, visit_str, &lk);
    if (rc && lk.found)
        slh_strlcpy(out, buf.data ? buf.data : "", out_size);
    sb_free(&buf);
    return rc && lk.found;
}

typedef struct {
    const char *want;
    int         value;
    int         found;
} bool_lookup_t;

static int visit_bool(const char *key, const char *value, void *ctx)
{
    bool_lookup_t *lk = ctx;

    if (strcmp(key, lk->want) != 0)
        return 0;
    if (slh_starts_with(value, "true") || slh_starts_with(value, "\"true\"")) {
        lk->value = 1;
        lk->found = 1;
        return 1;
    }
    if (slh_starts_with(value, "false") || slh_starts_with(value, "\"false\"")) {
        lk->value = 0;
        lk->found = 1;
        return 1;
    }
    return 0;
}

int json_flat_get_bool(const char *json, const char *key, int *out)
{
    bool_lookup_t lk;

    if (!json || !key || !out)
        return 0;
    lk.want = key;
    lk.value = 0;
    lk.found = 0;
    if (!json_flat_visit(json, visit_bool, &lk) || !lk.found)
        return 0;
    *out = lk.value;
    return 1;
}

typedef struct {
    const char *want;
    long long   value;
    int         found;
} int_lookup_t;

static int visit_int(const char *key, const char *value, void *ctx)
{
    int_lookup_t *lk = ctx;
    char *end = NULL;
    long long v;

    if (strcmp(key, lk->want) != 0)
        return 0;
    if (*value == '"')
        value++;
    v = strtoll(value, &end, 10);
    if (end == value)
        return 0;
    lk->value = v;
    lk->found = 1;
    return 1;
}

int json_flat_get_int(const char *json, const char *key, long long *out)
{
    int_lookup_t lk;

    if (!json || !key || !out)
        return 0;
    lk.want = key;
    lk.value = 0;
    lk.found = 0;
    if (!json_flat_visit(json, visit_int, &lk) || !lk.found)
        return 0;
    *out = lk.value;
    return 1;
}
