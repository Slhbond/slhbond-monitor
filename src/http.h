/*
 * http.h - a small HTTP/1.1 server: parsed requests, buffered responses,
 * and a fixed pool of accept()ing worker threads.
 *
 * Design notes for maintainers:
 *   - one listening socket, every worker blocks in accept(); the kernel
 *     distributes connections, so there is no hand-off queue to get wrong;
 *   - each connection is handled start-to-finish by one thread, with
 *     keep-alive up to a bounded number of requests and an idle timeout;
 *   - the router lives in router.c and never touches sockets.
 */
#ifndef SLH_HTTP_H
#define SLH_HTTP_H

#include <stddef.h>

#include "config.h"
#include "util.h"

#define SLH_MAX_HEADERS 32
#define SLH_HEADER_NAME_LEN 64
#define SLH_HEADER_VALUE_LEN 512
#define SLH_MAX_TARGET_LEN 2048
#define SLH_MAX_PATH_LEN 1024

/* ------------------------------------------------------------------ */
/* Request                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[SLH_HEADER_NAME_LEN];
    char value[SLH_HEADER_VALUE_LEN];
} http_header_t;

typedef struct {
    char          method[16];
    char          target[SLH_MAX_TARGET_LEN];  /* raw target incl. query      */
    char          path[SLH_MAX_PATH_LEN];      /* percent-decoded, no query   */
    char          query[SLH_MAX_PATH_LEN];     /* raw query string            */
    char          version[16];
    http_header_t headers[SLH_MAX_HEADERS];
    size_t        header_count;
    char          client_ip[64];
} http_request_t;

/** Case-insensitive header lookup; NULL when absent. */
const char *http_request_header(const http_request_t *req, const char *name);

/** Fetch one query parameter (percent-decoded). Returns 1 when present. */
int http_request_query_get(const http_request_t *req, const char *key,
                           char *out, size_t out_size);

/** 1 when the query string carries `key` with a truthy value (1/true/yes). */
int http_request_query_flag(const http_request_t *req, const char *key);

/* ------------------------------------------------------------------ */
/* Response                                                            */
/* ------------------------------------------------------------------ */

#define SLH_MAX_EXTRA_HEADERS 8

typedef struct {
    int      status;
    char     content_type[128];
    char     cache_control[128];
    strbuf_t body;
    int      keep_alive;
    struct {
        char name[SLH_HEADER_NAME_LEN];
        char value[256];
    } extra[SLH_MAX_EXTRA_HEADERS];
    size_t   extra_count;
} http_response_t;

void http_response_init(http_response_t *resp);
void http_response_free(http_response_t *resp);

/** Replace the body. `content_type` may be NULL to keep the current one. */
void http_response_set_body(http_response_t *resp, const char *data, size_t len,
                            const char *content_type);

/** Replace the body with a NUL-terminated string. */
void http_response_set_text(http_response_t *resp, const char *text,
                            const char *content_type);

void http_response_add_header(http_response_t *resp, const char *name,
                              const char *value);

/** Send a JSON document held in a strbuf. Sets status only when it is unset. */
void http_response_set_json(http_response_t *resp, const strbuf_t *sb);

/** Send a JSON error object: {"error":{"code":...,"message":...}} */
void http_response_set_json_error(http_response_t *resp, int status,
                                  const char *code, const char *message);

/** Plain-text error, used before the router takes over. */
void http_response_set_error(http_response_t *resp, int status, const char *message);

const char *http_status_text(int status);

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

typedef struct http_server http_server_t;

typedef void (*http_dispatch_fn)(const http_request_t *req,
                                 http_response_t *resp, void *ctx);

/**
 * Create and bind the listener.
 * Returns NULL when the socket cannot be prepared; `err` then holds a
 * human-readable reason.
 */
http_server_t *http_server_create(const config_t *cfg, http_dispatch_fn dispatch,
                                  void *ctx, char *err, size_t err_size);

/** Accept-and-serve loop; blocks until http_server_stop() is called. */
int http_server_run(http_server_t *srv);

/** Ask the server to stop. Safe to call from a signal handler or any thread. */
void http_server_stop(http_server_t *srv);

/** The bound port (useful when the config asked for port 0). */
int http_server_port(const http_server_t *srv);

void http_server_destroy(http_server_t *srv);

#endif /* SLH_HTTP_H */
