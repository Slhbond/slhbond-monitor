/*
 * util.c - implementation of the shared helpers declared in util.h.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static void oom(size_t n)
{
    fprintf(stderr, "slhbond-monitor: out of memory allocating %zu bytes\n", n);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom(n);
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        oom(n * size);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        oom(n);
    return q;
}

char *xstrdup(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

size_t slh_strlcpy(char *dst, const char *src, size_t dst_size)
{
    size_t len = src ? strlen(src) : 0;

    if (dst_size == 0)
        return len;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    if (len >= dst_size) {
        memcpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
        memcpy(dst, src, len + 1);
    }
    return len;
}

size_t slh_strlcat(char *dst, const char *src, size_t dst_size)
{
    size_t dlen = strlen(dst);
    size_t slen = src ? strlen(src) : 0;

    if (dlen >= dst_size)
        return dst_size + slen;
    return dlen + slh_strlcpy(dst + dlen, src, dst_size - dlen);
}

char *slh_trim(char *dst)
{
    char *end;

    if (!dst)
        return NULL;
    while (*dst && isspace((unsigned char)*dst))
        dst++;
    end = dst + strlen(dst);
    while (end > dst && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return dst;
}

int slh_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int slh_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int slh_ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);

    return ls >= lx && memcmp(s + ls - lx, suffix, lx) == 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void slh_url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;

    if (dst_size == 0)
        return;
    while (*src && out + 1 < dst_size) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            dst[out++] = (char)((hexval(src[1]) << 4) | hexval(src[2]));
            src += 3;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

void slh_format_bytes(uint64_t bytes, char *out, size_t out_size)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(out, out_size, "%llu %s", (unsigned long long)bytes, units[unit]);
    else
        snprintf(out, out_size, "%.1f %s", value, units[unit]);
}

void slh_format_uptime(uint64_t seconds, char *out, size_t out_size)
{
    uint64_t days = seconds / 86400;
    uint64_t hours = (seconds % 86400) / 3600;
    uint64_t mins = (seconds % 3600) / 60;

    if (days > 0)
        snprintf(out, out_size, "%llu 天 %llu 小时 %llu 分",
                 (unsigned long long)days, (unsigned long long)hours,
                 (unsigned long long)mins);
    else if (hours > 0)
        snprintf(out, out_size, "%llu 小时 %llu 分",
                 (unsigned long long)hours, (unsigned long long)mins);
    else if (mins > 0)
        snprintf(out, out_size, "%llu 分", (unsigned long long)mins);
    else
        snprintf(out, out_size, "%llu 秒", (unsigned long long)(seconds % 60));
}

/* ------------------------------------------------------------------ */
/* strbuf                                                              */
/* ------------------------------------------------------------------ */

#define SB_MIN_CAP 128u

void sb_init(strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb_init(sb);
}

void sb_reset(strbuf_t *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

int sb_reserve(strbuf_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;

    if (need <= sb->cap)
        return 0;
    cap = sb->cap ? sb->cap : SB_MIN_CAP;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {  /* overflow guard */
            cap = need;
            break;
        }
        cap *= 2;
    }
    sb->data = xrealloc(sb->data, cap);
    sb->cap = cap;
    if (sb->len == 0)
        sb->data[0] = '\0';
    return 0;
}

int sb_appendn(strbuf_t *sb, const char *s, size_t n)
{
    if (n == 0)
        return 0;
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_append(strbuf_t *sb, const char *s)
{
    return s ? sb_appendn(sb, s, strlen(s)) : 0;
}

int sb_appendc(strbuf_t *sb, char c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_vappendf(strbuf_t *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    int needed;

    va_copy(ap2, ap);
    needed = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (needed < 0)
        return -1;

    sb_reserve(sb, (size_t)needed);
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap);
    sb->len += (size_t)needed;
    return 0;
}

int sb_appendf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = sb_vappendf(sb, fmt, ap);
    va_end(ap);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Files and time                                                      */
/* ------------------------------------------------------------------ */

ssize_t slh_read_file(const char *path, char *buf, size_t buf_size)
{
    FILE *fp;
    size_t n;

    if (!buf || buf_size == 0)
        return -1;
    buf[0] = '\0';
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    n = fread(buf, 1, buf_size - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return (ssize_t)n;
}

int slh_file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t slh_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t slh_now_sec(void)
{
    return (int64_t)time(NULL);
}

void slh_rfc3339_local(time_t t, char *out, size_t out_size)
{
    struct tm tm_buf;
    char offset[16];
    long gmtoff = 0;

    if (!localtime_r(&t, &tm_buf)) {
        slh_strlcpy(out, "", out_size);
        return;
    }
#if defined(__USE_MISC) || defined(__GLIBC__)
    gmtoff = tm_buf.tm_gmtoff;
#endif
    {
        /* Real-world zones stay inside +/-18h; clamp so the field width is
         * provably sufficient for the compiler's format checker. */
        long off_min = gmtoff / 60;
        char sign = off_min < 0 ? '-' : '+';
        int  hours;
        int  minutes;

        if (off_min < 0)
            off_min = -off_min;
        if (off_min > 18 * 60)
            off_min = 18 * 60;
        hours = (int)(off_min / 60);
        minutes = (int)(off_min % 60);
        snprintf(offset, sizeof(offset), "%c%02d:%02d", sign, hours, minutes);
    }
    snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d%s",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, offset);
}

/* ------------------------------------------------------------------ */
/* External commands                                                   */
/* ------------------------------------------------------------------ */

int slh_is_executable(const char *path)
{
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

/* ------------------------------------------------------------------ */
/* Small kernel-interface readers                                      */
/* ------------------------------------------------------------------ */

int slh_read_text(const char *path, char *out, size_t out_size)
{
    FILE  *fp;
    size_t len;

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        out[0] = '\0';
        return 0;
    }
    fclose(fp);

    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == '\0'))
        out[--len] = '\0';
    return out[0] != '\0';
}

uint64_t slh_read_u64(const char *path, uint64_t fallback)
{
    char   buf[64];
    char  *end = NULL;
    unsigned long long value;

    if (!slh_read_text(path, buf, sizeof(buf)))
        return fallback;
    value = strtoull(buf, &end, 10);
    if (end == buf)
        return fallback;
    return (uint64_t)value;
}

int64_t slh_read_i64(const char *path, int64_t fallback)
{
    char  buf[64];
    char *end = NULL;
    long long value;

    if (!slh_read_text(path, buf, sizeof(buf)))
        return fallback;
    value = strtoll(buf, &end, 10);
    if (end == buf)
        return fallback;
    return (int64_t)value;
}

uint64_t slh_read_u64_auto(const char *path, uint64_t fallback)
{
    char   buf[64];
    char  *end = NULL;
    unsigned long long value;

    if (!slh_read_text(path, buf, sizeof(buf)))
        return fallback;
    value = strtoull(buf, &end, 0);      /* base 0: honours a 0x prefix */
    if (end == buf)
        return fallback;
    return (uint64_t)value;
}

int slh_readlink_basename(const char *path, char *out, size_t out_size)
{
    char   target[512];
    char  *slash;
    ssize_t n;

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0)
        return 0;
    target[n] = '\0';

    slash = strrchr(target, '/');
    slh_strlcpy(out, slash ? slash + 1 : target, out_size);
    return out[0] != '\0';
}

int slh_devicetree_string(const char *path, int index, char *out, size_t out_size)
{
    FILE  *fp;
    char   buffer[2048];
    size_t len;
    size_t offset = 0;
    int    current = 0;

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';

    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    len = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[len] = '\0';

    while (offset < len) {
        const char *entry = buffer + offset;
        size_t      entry_len = strnlen(entry, len - offset);

        if (entry_len == 0)
            break;
        if (current == index) {
            slh_strlcpy(out, entry, out_size);
            return out[0] != '\0';
        }
        current++;
        offset += entry_len + 1;
    }
    return 0;
}

int slh_run_command(const char *const argv[], char *out, size_t out_size,
                    int timeout_ms)
{
    int     pipefd[2];
    pid_t   pid;
    size_t  used = 0;
    int     timed_out = 0;
    int     status = 0;
    int64_t deadline;

    if (out && out_size)
        out[0] = '\0';
    if (!argv || !argv[0] || !out || out_size == 0)
        return -1;

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: merge stderr into stdout and become a process-group leader
         * so a timeout can take down anything it spawned in turn. */
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        if (pipefd[1] > STDERR_FILENO)
            close(pipefd[1]);
        setpgid(0, 0);
        /*
         * execv(3)'s prototype predates const-correctness: it takes
         * char *const[] even though it never writes through it. The cast is
         * the usual workaround and is safe, so the warning is suppressed for
         * this one call rather than weakening the API with a non-const
         * parameter.
         */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wcast-qual"
#endif
        execv(argv[0], (char *const *)argv);
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
        _exit(127);
    }

    close(pipefd[1]);
    setpgid(pid, pid);          /* harmless if the child already exec'd */

    deadline = slh_monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 1000);

    for (;;) {
        struct pollfd pfd;
        char          scratch[1024];
        char         *dst;
        size_t        room;
        int64_t       left;
        ssize_t       n;
        int           rc;

        left = deadline - slh_monotonic_ms();
        if (left <= 0) {
            timed_out = 1;
            break;
        }

        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, (int)left);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0) {
            timed_out = 1;
            break;
        }

        /* Keep draining even once the caller's buffer is full, otherwise the
         * child would block on write and we would wait for it forever. */
        room = (used + 1 < out_size) ? (out_size - used - 1) : 0;
        if (room > 0) {
            dst = out + used;
        } else {
            dst = scratch;
            room = sizeof(scratch);
        }

        n = read(pipefd[0], dst, room);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (n == 0)
            break;                       /* EOF: child closed the pipe */
        if (dst != scratch)
            used += (size_t)n;
    }

    out[used] = '\0';
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (timed_out)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}
