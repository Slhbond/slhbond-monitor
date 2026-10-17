/*
 * router.h - map an HTTP request onto an API handler or a static asset.
 */
#ifndef SLH_ROUTER_H
#define SLH_ROUTER_H

#include "http.h"

typedef void (*route_handler_fn)(const http_request_t *req,
                                 http_response_t *resp, void *ctx);

typedef struct {
    const char    *method;   /* "GET", "HEAD", or "*" for any method        */
    const char    *path;     /* exact path, or a prefix when it ends in '*' */
    route_handler_fn handler;
} route_entry_t;

typedef struct {
    const route_entry_t *entries;
    size_t               count;
    char                 doc_root[512];
    /**
     * Opaque context forwarded to every route handler. The router itself must
     * never be used as the handler context: handlers expect their own type
     * (see api_context_t), and handing them the router corrupts the table.
     */
    void                *handler_ctx;
} router_t;

void router_init(router_t *r, const route_entry_t *entries, size_t count,
                 const char *doc_root, void *handler_ctx);

/** http_dispatch_fn suitable for http_server_create(). */
void router_dispatch(const http_request_t *req, http_response_t *resp, void *ctx);

#endif /* SLH_ROUTER_H */
