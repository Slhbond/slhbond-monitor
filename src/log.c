/*
 * log.c - levelled logging with a mutex so worker threads stay readable.
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static log_level_t     g_level = LOG_LEVEL_INFO;
static FILE           *g_fp = NULL;   /* NULL means stderr only */

int log_init(log_level_t level, const char *file_path)
{
    g_level = level;
    if (file_path && file_path[0]) {
        FILE *fp = fopen(file_path, "a");
        if (!fp)
            return -1;
        setvbuf(fp, NULL, _IOLBF, 0);
        g_fp = fp;
    }
    return 0;
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

int log_level_parse(const char *name, log_level_t *out)
{
    if (slh_strcasecmp(name, "error") == 0)
        *out = LOG_LEVEL_ERROR;
    else if (slh_strcasecmp(name, "warn") == 0 || slh_strcasecmp(name, "warning") == 0)
        *out = LOG_LEVEL_WARN;
    else if (slh_strcasecmp(name, "info") == 0)
        *out = LOG_LEVEL_INFO;
    else if (slh_strcasecmp(name, "debug") == 0)
        *out = LOG_LEVEL_DEBUG;
    else
        return -1;
    return 0;
}

const char *log_level_name(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_INFO:  return "INFO ";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    default:              return "?????";
    }
}

void log_write(log_level_t level, const char *fmt, ...)
{
    char stamp[80];
    char message[1024];
    struct timespec ts;
    struct tm tm_buf;
    va_list ap;

    if (level > g_level)
        return;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_buf);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(ts.tv_nsec / 1000000));

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "%s [%s] %s\n", stamp, log_level_name(level), message);
    fflush(stderr);
    if (g_fp) {
        fprintf(g_fp, "%s [%s] %s\n", stamp, log_level_name(level), message);
        fflush(g_fp);
    }
    pthread_mutex_unlock(&g_lock);
}
