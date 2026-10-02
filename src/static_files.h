/*
 * static_files.h - serve the dashboard's HTML/CSS/JS from a document root.
 */
#ifndef SLH_STATIC_FILES_H
#define SLH_STATIC_FILES_H

#include "http.h"

/** MIME type for a path's extension; never returns NULL. */
const char *static_files_mime_type(const char *path);

/**
 * Serve `url_path` from `doc_root`.
 *
 * Returns 0 when a response was produced (200, 304, 403, 404 or 500) so the
 * caller knows the request has been handled.
 */
int static_files_serve(const char *doc_root, const char *url_path,
                       const http_request_t *req, http_response_t *resp);

#endif /* SLH_STATIC_FILES_H */
