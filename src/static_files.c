/*
 * static_files.c - document-root file serving with traversal protection.
 *
 * Every request is resolved with realpath(3) and must land inside the
 * canonical document root, so "../" tricks, symlink escapes and encoded
 * separators all fail closed with 403/404 instead of leaking files.
 */
#include "static_files.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"

#define MAX_ASSET_BYTES (8u * 1024u * 1024u)

typedef struct {
    const char *extension;
    const char *mime;
} mime_entry_t;

static const mime_entry_t g_mime[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "application/javascript; charset=utf-8" },
    { "mjs",  "application/javascript; charset=utf-8" },
    { "json", "application/json; charset=utf-8" },
    { "svg",  "image/svg+xml" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "webp", "image/webp" },
    { "ico",  "image/x-icon" },
    { "woff", "font/woff" },
    { "woff2", "font/woff2" },
    { "ttf",  "font/ttf" },
    { "txt",  "text/plain; charset=utf-8" },
    { "map",  "application/json; charset=utf-8" },
    { NULL,   NULL }
};

const char *static_files_mime_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    size_t i;

    if (!dot)
        return "application/octet-stream";
    dot++;
    for (i = 0; g_mime[i].extension; i++) {
        if (slh_strcasecmp(dot, g_mime[i].extension) == 0)
            return g_mime[i].mime;
    }
    return "application/octet-stream";
}

/** 1 when `child` is `root` itself or lives inside it. */
static int is_inside(const char *root, const char *child)
{
    size_t root_len = strlen(root);

    if (strncmp(root, child, root_len) != 0)
        return 0;
    return child[root_len] == '\0' || child[root_len] == '/';
}

static int read_whole_file(const char *path, strbuf_t *out)
{
    FILE  *fp;
    char   chunk[8192];
    size_t total = 0;

    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), fp);

        if (n > 0) {
            total += n;
            if (total > MAX_ASSET_BYTES) {
                fclose(fp);
                return -1;
            }
            sb_appendn(out, chunk, n);
        }
        if (n < sizeof(chunk))
            break;
    }
    fclose(fp);
    return 0;
}

/** Compare the client's If-None-Match against our weak validator. */
static int etag_matches(const http_request_t *req, const char *etag)
{
    const char *inm = http_request_header(req, "If-None-Match");

    if (!inm)
        return 0;
    return strstr(inm, etag) != NULL;
}

int static_files_serve(const char *doc_root, const char *url_path,
                       const http_request_t *req, http_response_t *resp)
{
    char        root[PATH_MAX];
    char        candidate[PATH_MAX];
    char        resolved[PATH_MAX];
    struct stat st;
    const char *rel = url_path;

    if (!realpath(doc_root, root)) {
        LOG_WARN("document root '%s' is not accessible: %s", doc_root,
                 strerror(errno));
        http_response_set_error(resp, 404,
                                "web assets not installed (check doc_root)");
        return 0;
    }

    while (*rel == '/')
        rel++;
    if (rel[0] == '\0')
        rel = "index.html";

    if (snprintf(candidate, sizeof(candidate), "%s/%s", root, rel) >=
        (int)sizeof(candidate)) {
        http_response_set_error(resp, 414, "path too long");
        return 0;
    }

    if (!realpath(candidate, resolved)) {
        http_response_set_json_error(resp, 404, "not_found", "请求的资源不存在");
        return 0;
    }
    if (!is_inside(root, resolved)) {
        LOG_WARN("blocked traversal attempt: %s -> %s", url_path, resolved);
        http_response_set_json_error(resp, 403, "forbidden", "禁止访问该路径");
        return 0;
    }

    /* A directory request maps onto its index.html. */
    if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
        char index_path[PATH_MAX];

        if (snprintf(index_path, sizeof(index_path), "%s/index.html", resolved) >=
            (int)sizeof(index_path) || !realpath(index_path, resolved)) {
            http_response_set_json_error(resp, 404, "not_found", "请求的资源不存在");
            return 0;
        }
    }

    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        http_response_set_json_error(resp, 404, "not_found", "请求的资源不存在");
        return 0;
    }
    if (st.st_size > (off_t)MAX_ASSET_BYTES) {
        http_response_set_error(resp, 500, "asset is too large to serve");
        return 0;
    }

    {
        char etag[96];

        snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
                 (unsigned long long)st.st_mtime, (unsigned long long)st.st_size);
        http_response_add_header(resp, "ETag", etag);

        if (etag_matches(req, etag)) {
            resp->status = 304;
            sb_reset(&resp->body);
            slh_strlcpy(resp->cache_control, "public, max-age=60",
                        sizeof(resp->cache_control));
            return 0;
        }
    }

    sb_reset(&resp->body);
    if (read_whole_file(resolved, &resp->body) != 0) {
        http_response_set_error(resp, 500, "cannot read asset");
        return 0;
    }

    resp->status = 200;
    slh_strlcpy(resp->content_type, static_files_mime_type(resolved),
                sizeof(resp->content_type));
    slh_strlcpy(resp->cache_control, "public, max-age=60",
                sizeof(resp->cache_control));
    return 0;
}
