/*
 * httpclient.h - minimal blocking HTTP/1.1 GET client.
 *
 * Used only by the update checker to fetch a release manifest from a URL.
 * Plain http:// is supported; https:// is reported as unsupported because
 * Slhbond-Monitor deliberately links against nothing but libc and pthreads.
 *
 * NOTE: the response type here is deliberately named `http_client_response_t`
 * to stay distinct from the *server* side `http_response_t` in http.h.
 */
#ifndef SLH_HTTPCLIENT_H
#define SLH_HTTPCLIENT_H

#include <stddef.h>

typedef struct {
    int    status;                 /* HTTP status code, 0 on transport error */
    char  *body;                   /* NUL-terminated, may be NULL            */
    size_t body_len;
    char   content_type[128];
    char   error[192];
} http_client_response_t;

/**
 * Perform a GET request.
 * Returns 0 when a complete response was read, -1 on any transport error
 * (in which case `resp->error` describes it).
 */
int http_get(const char *url, http_client_response_t *resp, int timeout_ms);

void http_client_response_free(http_client_response_t *resp);

/* ------------------------------------------------------------------ */
/* URL fetching, https included                                        */
/* ------------------------------------------------------------------ */

/*
 * GitHub only serves over https, and this project links against nothing but
 * libc and pthreads - there is no TLS stack inside it. Rather than grow one,
 * an external downloader is used when the scheme is https.
 *
 * Two consequences worth knowing:
 *   - curl(1) or wget(1) must be installed for https to work at all;
 *     http:// keeps working through the built-in client without either.
 *   - the download is verified by SHA-256 against the manifest, so a
 *     tampered mirror is detected even though the transport is only as
 *     trustworthy as the system's CA store.
 */

/** 1 when an external https downloader was found; its path goes into `tool`. */
int http_https_tool(char *tool, size_t tool_size);

/**
 * Fetch a URL into a NUL-terminated buffer.
 * Returns 0 on success, -1 on failure with `err` filled in.
 */
int http_fetch(const char *url, char *out, size_t out_size, int timeout_ms,
               char *err, size_t err_size);

/** Fetch a URL straight to a file. Returns 0 on success. */
int http_fetch_file(const char *url, const char *path, int timeout_ms,
                    char *err, size_t err_size);

#endif /* SLH_HTTPCLIENT_H */
