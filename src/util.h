/*
 * util.h - small shared helpers: memory, strings, dynamic buffers, files, time.
 *
 * Slhbond-Monitor / pure C11, no third-party dependencies.
 */
#ifndef SLH_UTIL_H
#define SLH_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#  define SLH_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#  define SLH_UNUSED(x) ((void)(x))
#else
#  define SLH_PRINTF(fmt_idx, arg_idx)
#  define SLH_UNUSED(x) ((void)(x))
#endif

/* ------------------------------------------------------------------ */
/* Memory: allocation failures are fatal; this is a long-running daemon */
/* and limping along after an OOM only produces corrupted state.        */
/* ------------------------------------------------------------------ */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

/** Copy at most dst_size-1 bytes, always NUL-terminating. Returns strlen(src). */
size_t slh_strlcpy(char *dst, const char *src, size_t dst_size);

/** Append src to dst (dst_size includes the NUL). Returns the new length. */
size_t slh_strlcat(char *dst, const char *src, size_t dst_size);

/** Trim leading/trailing ASCII whitespace in place. Returns dst. */
char *slh_trim(char *dst);

/** Case-insensitive comparison, ASCII only. */
int slh_strcasecmp(const char *a, const char *b);

/** 1 when `s` begins with `prefix`. */
int slh_starts_with(const char *s, const char *prefix);

/** 1 when `s` ends with `suffix`. */
int slh_ends_with(const char *s, const char *suffix);

/** Percent-decode `src` into `dst` (at most dst_size-1 bytes + NUL). */
void slh_url_decode(const char *src, char *dst, size_t dst_size);

/** Human-readable byte count, e.g. "1.5 GiB". */
void slh_format_bytes(uint64_t bytes, char *out, size_t out_size);

/** Human-readable duration in Chinese, e.g. "3 天 4 小时 12 分". */
void slh_format_uptime(uint64_t seconds, char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* strbuf: growable byte buffer used to build HTTP bodies and JSON      */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;  /* always NUL-terminated once initialised */
    size_t len;
    size_t cap;
} strbuf_t;

void sb_init(strbuf_t *sb);
void sb_free(strbuf_t *sb);
void sb_reset(strbuf_t *sb);

/** Ensure room for `extra` more bytes plus the NUL terminator. */
int sb_reserve(strbuf_t *sb, size_t extra);
int sb_append(strbuf_t *sb, const char *s);
int sb_appendn(strbuf_t *sb, const char *s, size_t n);
int sb_appendc(strbuf_t *sb, char c);
int sb_appendf(strbuf_t *sb, const char *fmt, ...) SLH_PRINTF(2, 3);
int sb_vappendf(strbuf_t *sb, const char *fmt, va_list ap);

/* ------------------------------------------------------------------ */
/* Files and time                                                      */
/* ------------------------------------------------------------------ */

/**
 * Read a whole file into `buf` (NUL-terminated, truncated to buf_size-1).
 * Returns the number of bytes read, or -1 when the file cannot be opened.
 */
ssize_t slh_read_file(const char *path, char *buf, size_t buf_size);

/** 1 when `path` exists and is a regular file. */
int slh_file_exists(const char *path);

/** Monotonic milliseconds since an arbitrary epoch; for timeouts. */
int64_t slh_monotonic_ms(void);

/** Wall-clock seconds since the Unix epoch. */
int64_t slh_now_sec(void);

/** Local time formatted as RFC3339, e.g. "2026-10-01T16:26:22+08:00". */
void slh_rfc3339_local(time_t t, char *out, size_t out_size);

/**
 * Run an external command and capture its merged stdout+stderr.
 *
 * `argv` is a NULL-terminated argument vector whose first element is an
 * absolute path (the daemon runs with a minimal environment, so PATH lookup
 * is deliberately not used). The child is placed in its own process group so
 * a timeout kills its whole tree.
 *
 * Returns the child's exit status, or -1 when it could not be started or had
 * to be killed after `timeout_ms`. Output is always NUL-terminated and
 * truncated to `out_size`.
 */
int slh_run_command(const char *const argv[], char *out, size_t out_size,
                    int timeout_ms);

/** 1 when `path` exists and is executable. */
int slh_is_executable(const char *path);

/* ------------------------------------------------------------------ */
/* Small kernel-interface readers (sysfs / procfs)                      */
/* ------------------------------------------------------------------ */

/**
 * Read a small text file and trim trailing newline / NUL bytes.
 * Returns 1 on success, 0 when the file is missing or empty.
 */
int slh_read_text(const char *path, char *out, size_t out_size);

/** Read a decimal integer from a file; `fallback` when unreadable. */
uint64_t slh_read_u64(const char *path, uint64_t fallback);

/** Read a signed decimal integer from a file; `fallback` when unreadable. */
int64_t slh_read_i64(const char *path, int64_t fallback);

/**
 * Read an integer written in C notation (base auto-detected, so both "0x01"
 * and "42" work). eMMC health attributes use the "0xNN" form, which plain
 * base-10 parsing would silently turn into 0.
 */
uint64_t slh_read_u64_auto(const char *path, uint64_t fallback);

/**
 * Copy the basename of a symlink's target into `out`.
 * e.g. "/sys/devices/.../fde60000.gpu/driver" -> "panfrost".
 * Returns 1 on success.
 */
int slh_readlink_basename(const char *path, char *out, size_t out_size);

/**
 * Read a NUL-separated device-tree string-list property (e.g. "compatible")
 * and copy entry `index` into `out`. Returns 1 on success.
 */
int slh_devicetree_string(const char *path, int index, char *out, size_t out_size);

#endif /* SLH_UTIL_H */
