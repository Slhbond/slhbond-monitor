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

#endif /* SLH_HTTPCLIENT_H */
