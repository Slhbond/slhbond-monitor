/*
 * log.h - levelled logging to stderr and/or a file.
 */
#ifndef SLH_LOG_H
#define SLH_LOG_H

#include "util.h"

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/**
 * Initialise logging.
 *   level     - messages above this level are dropped
 *   file_path - append to this file; NULL or "" logs to stderr only
 * Returns 0 on success, -1 when the file cannot be opened.
 */
int log_init(log_level_t level, const char *file_path);

void log_close(void);

/** Parse "error" | "warn" | "info" | "debug"; returns -1 when unknown. */
int log_level_parse(const char *name, log_level_t *out);

const char *log_level_name(log_level_t level);

void log_write(log_level_t level, const char *fmt, ...) SLH_PRINTF(2, 3);

#define LOG_ERROR(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif /* SLH_LOG_H */
