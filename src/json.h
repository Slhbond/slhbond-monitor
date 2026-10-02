/*
 * json.h - tiny JSON writer helpers plus a flat-object reader.
 *
 * The writer is what the HTTP API uses to build responses; it is deliberately
 * minimal (no DOM, no allocation) so responses stream straight into a strbuf.
 *
 * The reader only understands a *flat* JSON object whose values are strings,
 * numbers, booleans, null, or nested containers that are skipped over. That is
 * exactly the shape of the release manifest we fetch, and it keeps this file
 * small and auditable.
 */
#ifndef SLH_JSON_H
#define SLH_JSON_H

#include <stddef.h>

#include "util.h"

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

/** Append `s` with JSON string escaping (quotes, backslashes, controls). */
void json_escape(strbuf_t *sb, const char *s);

/**
 * Emit only the `"key":` prefix (comma included when needed) so a caller can
 * append a pre-rendered value such as an array. Never leave a trailing comma
 * after the raw value - the next json_add_* emits its own separator.
 *
 * The json_add_* helpers below all use this to track `first`.
 */
void json_key(strbuf_t *sb, const char *key, int *first);

void json_add_str(strbuf_t *sb, const char *key, const char *val, int *first);
void json_add_int(strbuf_t *sb, const char *key, long long val, int *first);
void json_add_uint(strbuf_t *sb, const char *key, unsigned long long val, int *first);
void json_add_bool(strbuf_t *sb, const char *key, int val, int *first);
void json_add_double(strbuf_t *sb, const char *key, double val, int *first);
void json_add_null(strbuf_t *sb, const char *key, int *first);
/** Insert a pre-rendered JSON fragment verbatim. */
void json_add_raw(strbuf_t *sb, const char *key, const char *raw, int *first);
/** Insert an array of strings. A NULL `items` emits an empty array. */
void json_add_str_array(strbuf_t *sb, const char *key,
                        const char *const *items, size_t count, int *first);

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

/**
 * Fetch a top-level string member.
 * Returns 1 and NUL-terminates `out` on success, 0 when absent or not a string.
 */
int json_flat_get_str(const char *json, const char *key, char *out, size_t out_size);

/**
 * Fetch a top-level boolean member. Accepts true/false and "true"/"false".
 * Returns 1 on success, 0 when absent or unparsable.
 */
int json_flat_get_bool(const char *json, const char *key, int *out);

/**
 * Fetch a top-level numeric member.
 * Returns 1 on success, 0 when absent or unparsable.
 */
int json_flat_get_int(const char *json, const char *key, long long *out);

#endif /* SLH_JSON_H */
