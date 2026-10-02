/*
 * router.c - route matching, method filtering and the static fallback.
 *
 * Method semantics:
 *   - OPTIONS always answers 204 with an accurate Allow header;
 *   - HEAD is served by the GET handler (the response writer drops the body);
 *   - a path that exists but not for the requested method yields 405 + Allow,
 *     which is what a client needs to recover;
 *   - anything else under /api/ is 404, and anything else at all falls through
 *     to the static document root.
 */
#include "router.h"

#include <string.h>

#include "log.h"
#include "static_files.h"

void router_init(router_t *r, const route_entry_t *entries, size_t count,
                 const char *doc_root, void *handler_ctx)
{
    r->entries = entries;
    r->count = count;
    slh_strlcpy(r->doc_root, doc_root, sizeof(r->doc_root));
    r->handler_ctx = handler_ctx;
}

static int path_matches(const char *route_path, const char *request_path)
{
    size_t len = strlen(route_path);

    if (len > 0 && route_path[len - 1] == '*')
        return strncmp(route_path, request_path, len - 1) == 0;
    return strcmp(route_path, request_path) == 0;
}

/** HEAD is just GET without a body. */
static int method_matches(const char *route_method, const char *request_method)
{
    if (strcmp(route_method, "*") == 0)
        return 1;
    if (strcmp(route_method, request_method) == 0)
        return 1;
    if (strcmp(route_method, "GET") == 0 && strcmp(request_method, "HEAD") == 0)
        return 1;
    return 0;
}

static int method_is_read(const char *method)
{
    return strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
}

static const route_entry_t *find_route(const router_t *r, const http_request_t *req)
{
    size_t i;

    for (i = 0; i < r->count; i++) {
        const route_entry_t *e = &r->entries[i];

        if (!method_matches(e->method, req->method))
            continue;
        if (path_matches(e->path, req->path))
            return e;
    }
    return NULL;
}

/**
 * Collect the methods registered for `path` into an Allow header value.
 * Returns 1 when at least one route matched, leaving `out` set to "" otherwise.
 */
static int build_allow(const router_t *r, const char *path, char *out, size_t out_size)
{
    size_t i;
    int    found = 0;

    out[0] = '\0';
    for (i = 0; i < r->count; i++) {
        const route_entry_t *e = &r->entries[i];
        size_t               k;
        int                  duplicate = 0;

        if (strcmp(e->method, "*") == 0)
            continue;
        if (!path_matches(e->path, path))
            continue;

        for (k = 0; k < i; k++) {
            const route_entry_t *prev = &r->entries[k];

            if (strcmp(prev->method, e->method) == 0 &&
                path_matches(prev->path, path)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        if (found)
            slh_strlcat(out, ", ", out_size);
        slh_strlcat(out, e->method, out_size);
        found = 1;
    }

    if (found)
        slh_strlcat(out, ", HEAD, OPTIONS", out_size);
    return found;
}

void router_dispatch(const http_request_t *req, http_response_t *resp, void *ctx)
{
    router_t            *r = ctx;
    const route_entry_t *route;
    char                 allow[96];

    if (strcmp(req->method, "OPTIONS") == 0) {
        build_allow(r, req->path, allow, sizeof(allow));
        resp->status = 204;
        if (allow[0] != '\0')
            http_response_add_header(resp, "Allow", allow);
        sb_reset(&resp->body);
        return;
    }

    route = find_route(r, req);
    if (route) {
        route->handler(req, resp, r->handler_ctx);
        return;
    }

    /* No handler. If the path is registered for some other method, say so
     * precisely instead of pretending the endpoint does not exist. */
    if (build_allow(r, req->path, allow, sizeof(allow))) {
        http_response_add_header(resp, "Allow", allow);
        http_response_set_json_error(resp, 405, "method_not_allowed",
                                     "该接口不支持此请求方法");
        return;
    }

    if (slh_starts_with(req->path, "/api/")) {
        http_response_set_json_error(resp, 404, "not_found", "接口不存在");
        return;
    }

    /* The dashboard is read-only apart from the disk health trigger. */
    if (!method_is_read(req->method)) {
        http_response_set_json_error(resp, 405, "method_not_allowed",
                                     "该服务只接受只读请求");
        return;
    }

    static_files_serve(r->doc_root, req->path, req, resp);
}
