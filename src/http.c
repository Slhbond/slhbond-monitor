/*
 * http.c - HTTP/1.1 request parsing, response writing and the worker pool.
 */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "log.h"

#define RECV_TIMEOUT_SEC 10
#define MAX_REQUEST_BYTES (16u * 1024u)
#define MAX_REQUESTS_PER_CONN 100

struct http_server {
    config_t        cfg;
    http_dispatch_fn dispatch;
    void           *ctx;
    int             listen_fd;
    volatile sig_atomic_t stop;
    pthread_t       workers[64];
    int             worker_count;
    int             bound_port;
};

/* ------------------------------------------------------------------ */
/* Response helpers                                                    */
/* ------------------------------------------------------------------ */

void http_response_init(http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = 200;
    resp->keep_alive = 1;
    slh_strlcpy(resp->content_type, "text/plain; charset=utf-8",
                sizeof(resp->content_type));
    slh_strlcpy(resp->cache_control, "no-store, no-cache, must-revalidate",
                sizeof(resp->cache_control));
    sb_init(&resp->body);
}

void http_response_free(http_response_t *resp)
{
    sb_free(&resp->body);
}

void http_response_set_body(http_response_t *resp, const char *data, size_t len,
                            const char *content_type)
{
    sb_reset(&resp->body);
    if (data && len)
        sb_appendn(&resp->body, data, len);
    if (content_type)
        slh_strlcpy(resp->content_type, content_type, sizeof(resp->content_type));
}

void http_response_set_text(http_response_t *resp, const char *text,
                            const char *content_type)
{
    http_response_set_body(resp, text, text ? strlen(text) : 0, content_type);
}

void http_response_add_header(http_response_t *resp, const char *name,
                              const char *value)
{
    if (resp->extra_count >= SLH_MAX_EXTRA_HEADERS)
        return;
    slh_strlcpy(resp->extra[resp->extra_count].name, name, SLH_HEADER_NAME_LEN);
    slh_strlcpy(resp->extra[resp->extra_count].value, value, 256);
    resp->extra_count++;
}

void http_response_set_json(http_response_t *resp, const strbuf_t *sb)
{
    sb_reset(&resp->body);
    if (sb && sb->data)
        sb_appendn(&resp->body, sb->data, sb->len);
    slh_strlcpy(resp->content_type, "application/json; charset=utf-8",
                sizeof(resp->content_type));
}

void http_response_set_json_error(http_response_t *resp, int status,
                                  const char *code, const char *message)
{
    strbuf_t sb;
    int      first = 1;

    sb_init(&sb);
    sb_append(&sb, "{\"error\":{");
    json_add_str(&sb, "code", code, &first);
    json_add_str(&sb, "message", message, &first);
    sb_append(&sb, "}}");

    resp->status = status;
    http_response_set_json(resp, &sb);
    sb_free(&sb);
}

void http_response_set_error(http_response_t *resp, int status, const char *message)
{
    strbuf_t sb;

    sb_init(&sb);
    sb_appendf(&sb, "%d %s\n%s\n", status, http_status_text(status), message);
    resp->status = status;
    http_response_set_body(resp, sb.data, sb.len, "text/plain; charset=utf-8");
    sb_free(&sb);
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Request helpers                                                     */
/* ------------------------------------------------------------------ */

const char *http_request_header(const http_request_t *req, const char *name)
{
    size_t i;

    for (i = 0; i < req->header_count; i++) {
        if (slh_strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/** Find `key` in a raw query string; returns the raw value or NULL. */
static const char *query_lookup(const char *query, const char *key, size_t *value_len)
{
    size_t key_len = strlen(key);
    const char *p = query;

    if (!query || !*query)
        return NULL;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));

        if (eq) {
            if ((size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
                if (value_len)
                    *value_len = (size_t)(end - eq - 1);
                return eq + 1;
            }
        } else if ((size_t)(end - p) == key_len && strncmp(p, key, key_len) == 0) {
            if (value_len)
                *value_len = 0;
            return end;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return NULL;
}

int http_request_query_get(const http_request_t *req, const char *key,
                           char *out, size_t out_size)
{
    size_t raw_len = 0;
    const char *raw = query_lookup(req->query, key, &raw_len);
    char encoded[SLH_MAX_PATH_LEN];

    if (out_size)
        out[0] = '\0';
    if (!raw)
        return 0;
    if (raw_len >= sizeof(encoded))
        raw_len = sizeof(encoded) - 1;
    memcpy(encoded, raw, raw_len);
    encoded[raw_len] = '\0';
    slh_url_decode(encoded, out, out_size);
    return 1;
}

int http_request_query_flag(const http_request_t *req, const char *key)
{
    char value[32];

    if (!http_request_query_get(req, key, value, sizeof(value)))
        return 0;
    if (value[0] == '\0')
        return 1;                       /* "?refresh" counts as set */
    if (strcmp(value, "0") == 0 || slh_strcasecmp(value, "false") == 0 ||
        slh_strcasecmp(value, "no") == 0)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Socket plumbing                                                     */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void format_http_date(char *out, size_t out_size)
{
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now = time(NULL);
    struct tm tm_buf;

    if (!gmtime_r(&now, &tm_buf)) {
        slh_strlcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", out_size);
        return;
    }
    snprintf(out, out_size, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm_buf.tm_wday % 7], tm_buf.tm_mday,
             months[tm_buf.tm_mon % 12], tm_buf.tm_year + 1900,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

static int write_response(int fd, const http_request_t *req, http_response_t *resp)
{
    strbuf_t head;
    char     date[64];
    int      is_head = strcmp(req->method, "HEAD") == 0;
    int      rc;

    format_http_date(date, sizeof(date));
    sb_init(&head);
    sb_appendf(&head, "HTTP/1.1 %d %s\r\n", resp->status,
               http_status_text(resp->status));
    sb_appendf(&head, "Date: %s\r\n", date);
    sb_appendf(&head, "Server: Slhbond-Monitor\r\n");
    sb_appendf(&head, "Content-Type: %s\r\n", resp->content_type);
    sb_appendf(&head, "Content-Length: %zu\r\n", resp->body.len);
    sb_appendf(&head, "Connection: %s\r\n", resp->keep_alive ? "keep-alive" : "close");
    sb_append(&head, "Cache-Control: ");
    sb_append(&head, resp->cache_control[0] ? resp->cache_control : "no-store");
    sb_append(&head, "\r\n");
    sb_append(&head, "X-Content-Type-Options: nosniff\r\n");
    {
        size_t i;
        for (i = 0; i < resp->extra_count; i++)
            sb_appendf(&head, "%s: %s\r\n", resp->extra[i].name, resp->extra[i].value);
    }
    sb_append(&head, "\r\n");

    rc = send_all(fd, head.data, head.len);
    if (rc == 0 && !is_head && resp->body.len > 0)
        rc = send_all(fd, resp->body.data, resp->body.len);

    sb_free(&head);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Request parsing                                                     */
/* ------------------------------------------------------------------ */

/** Split one header block (already isolated) into method/target/headers. */
static int parse_request_head(char *head, http_request_t *req, int *bad_request)
{
    char *line_end;
    char *cursor;

    *bad_request = 0;

    /* --- request line --------------------------------------------- */
    line_end = strstr(head, "\r\n");
    if (!line_end)
        line_end = strchr(head, '\n');
    if (!line_end) {
        *bad_request = 1;
        return -1;
    }
    *line_end = '\0';

    {
        char *method = head;
        char *target = strchr(method, ' ');
        char *version;

        if (!target) {
            *bad_request = 1;
            return -1;
        }
        *target++ = '\0';
        while (*target == ' ')
            target++;
        version = strchr(target, ' ');
        if (version) {
            *version++ = '\0';
            while (*version == ' ')
                version++;
            slh_strlcpy(req->version, version, sizeof(req->version));
        } else {
            slh_strlcpy(req->version, "HTTP/1.0", sizeof(req->version));
        }
        slh_strlcpy(req->method, method, sizeof(req->method));
        slh_strlcpy(req->target, target, sizeof(req->target));
    }

    /* --- split target into path + query --------------------------- */
    {
        const char *q = strchr(req->target, '?');

        if (q) {
            size_t path_len = (size_t)(q - req->target);
            char   raw_path[SLH_MAX_PATH_LEN];

            if (path_len >= sizeof(raw_path))
                path_len = sizeof(raw_path) - 1;
            memcpy(raw_path, req->target, path_len);
            raw_path[path_len] = '\0';
            slh_strlcpy(req->query, q + 1, sizeof(req->query));
            slh_url_decode(raw_path, req->path, sizeof(req->path));
        } else {
            slh_url_decode(req->target, req->path, sizeof(req->path));
            req->query[0] = '\0';
        }
    }

    /* --- headers --------------------------------------------------- */
    cursor = line_end + 1;
    if (*cursor == '\n')
        cursor++;

    while (*cursor) {
        char  *eol = strstr(cursor, "\r\n");
        size_t skip = 2;
        char   saved;
        char  *colon;

        if (!eol) {
            eol = strchr(cursor, '\n');
            skip = 1;
        }
        if (!eol) {
            eol = cursor + strlen(cursor);
            skip = 0;
        }

        saved = *eol;
        *eol = '\0';

        colon = strchr(cursor, ':');
        if (colon && req->header_count < SLH_MAX_HEADERS) {
            http_header_t *h = &req->headers[req->header_count];

            *colon = '\0';
            slh_strlcpy(h->name, slh_trim(cursor), SLH_HEADER_NAME_LEN);
            slh_strlcpy(h->value, slh_trim(colon + 1), SLH_HEADER_VALUE_LEN);
            if (h->name[0])
                req->header_count++;
        }

        *eol = saved;
        if (skip == 0)
            break;
        cursor = eol + skip;
    }
    return 0;
}

static int read_request(int fd, char *buf, size_t buf_size, http_request_t *req,
                        int *bad_request)
{
    size_t used = 0;
    char  *head_end = NULL;
    char  *head_start;

    *bad_request = 0;

    while (used + 1 < buf_size) {
        ssize_t n = recv(fd, buf + used, buf_size - used - 1, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;                          /* timeout or hard error */
        }
        if (n == 0)
            return used == 0 ? 0 : -1;          /* clean close vs. truncation */
        used += (size_t)n;
        buf[used] = '\0';

        head_end = strstr(buf, "\r\n\r\n");
        if (head_end) {
            head_end += 4;
            break;
        }
        head_end = strstr(buf, "\n\n");
        if (head_end) {
            head_end += 2;
            break;
        }
    }

    if (!head_end) {
        *bad_request = 1;
        return -1;
    }

    head_start = buf;
    {
        char saved = *head_end;
        int  rc;

        *head_end = '\0';
        rc = parse_request_head(head_start, req, bad_request);
        *head_end = saved;
        return rc == 0 ? 1 : -1;
    }
}

/* ------------------------------------------------------------------ */
/* Connection handling                                                 */
/* ------------------------------------------------------------------ */

static void fill_client_ip(int fd, char *out, size_t out_size)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    void *addr;

    slh_strlcpy(out, "unknown", out_size);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return;
    if (ss.ss_family == AF_INET)
        addr = &((struct sockaddr_in *)&ss)->sin_addr;
    else if (ss.ss_family == AF_INET6)
        addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
    else
        return;
    if (!inet_ntop(ss.ss_family, addr, out, (socklen_t)out_size))
        slh_strlcpy(out, "unknown", out_size);
}

static void serve_connection(http_server_t *srv, int fd)
{
    char           *buf = xmalloc(MAX_REQUEST_BYTES);
    http_request_t *req = xcalloc(1, sizeof(http_request_t));
    int             served = 0;

    for (;;) {
        http_response_t resp;
        const char     *connection_header;
        int             bad_request = 0;
        int             rc;

        if (srv->stop || served >= MAX_REQUESTS_PER_CONN)
            break;

        memset(req, 0, sizeof(*req));
        rc = read_request(fd, buf, MAX_REQUEST_BYTES, req, &bad_request);
        if (rc <= 0) {
            if (rc == 0 && served == 0)
                LOG_DEBUG("client disconnected before sending a request");
            break;
        }
        served++;

        fill_client_ip(fd, req->client_ip, sizeof(req->client_ip));
        if (req->path[0] == '\0') {
            slh_strlcpy(req->path, "/", sizeof(req->path));
        }

        http_response_init(&resp);
        connection_header = http_request_header(req, "Connection");
        if (connection_header && slh_strcasecmp(connection_header, "close") == 0)
            resp.keep_alive = 0;
        if (strcmp(req->version, "HTTP/1.1") != 0) {
            if (!connection_header ||
                slh_strcasecmp(connection_header, "keep-alive") != 0)
                resp.keep_alive = 0;
        }
        /* Request bodies are never read, so a body would desynchronise the
         * next request on a kept-alive connection. Close instead. */
        if (http_request_header(req, "Content-Length") ||
            http_request_header(req, "Transfer-Encoding"))
            resp.keep_alive = 0;

        srv->dispatch(req, &resp, srv->ctx);

        if (write_response(fd, req, &resp) != 0) {
            http_response_free(&resp);
            LOG_DEBUG("failed to write response to %s", req->client_ip);
            break;
        }

        LOG_DEBUG("%s %s %d %s", req->method, req->target, resp.status, req->client_ip);

        {
            int keep = resp.keep_alive;
            http_response_free(&resp);
            if (!keep)
                break;
        }
    }

    free(req);
    free(buf);
}

static void *worker_main(void *arg)
{
    http_server_t *srv = arg;

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int       fd;

        if (srv->stop)
            break;

        fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (srv->stop || errno == EBADF || errno == EINVAL)
                break;                   /* listener was closed for shutdown */
            LOG_WARN("accept failed: %s", strerror(errno));
            continue;
        }

        {
            struct timeval tv;
            int            one = 1;

            tv.tv_sec = RECV_TIMEOUT_SEC;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }

        serve_connection(srv, fd);
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                    */
/* ------------------------------------------------------------------ */

http_server_t *http_server_create(const config_t *cfg, http_dispatch_fn dispatch,
                                  void *ctx, char *err, size_t err_size)
{
    http_server_t *srv;
    int            fd;
    int            one = 1;
    struct sockaddr_in addr;

    err[0] = '\0';
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, err_size, "socket() failed: %s", strerror(errno));
        return NULL;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    if (strcmp(cfg->bind_address, "0.0.0.0") == 0 || cfg->bind_address[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, cfg->bind_address, &addr.sin_addr) != 1) {
        snprintf(err, err_size, "invalid bind address '%s'", cfg->bind_address);
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(err, err_size, "bind %s:%d failed: %s", cfg->bind_address,
                 cfg->port, strerror(errno));
        close(fd);
        return NULL;
    }
    if (listen(fd, 128) != 0) {
        snprintf(err, err_size, "listen failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    srv = xcalloc(1, sizeof(*srv));
    srv->cfg = *cfg;
    srv->dispatch = dispatch;
    srv->ctx = ctx;
    srv->listen_fd = fd;
    srv->stop = 0;
    srv->worker_count = cfg->worker_threads;

    {
        struct sockaddr_in actual;
        socklen_t len = sizeof(actual);
        srv->bound_port = cfg->port;
        if (getsockname(fd, (struct sockaddr *)&actual, &len) == 0)
            srv->bound_port = ntohs(actual.sin_port);
    }
    return srv;
}

int http_server_run(http_server_t *srv)
{
    int i;

    for (i = 0; i < srv->worker_count; i++) {
        if (pthread_create(&srv->workers[i], NULL, worker_main, srv) != 0) {
            LOG_ERROR("could not start worker thread %d", i);
            srv->worker_count = i;
            break;
        }
    }
    if (srv->worker_count == 0) {
        LOG_ERROR("no worker threads could be started");
        return -1;
    }

    LOG_INFO("listening on %s:%d with %d worker thread(s)",
             srv->cfg.bind_address, srv->bound_port, srv->worker_count);

    for (i = 0; i < srv->worker_count; i++)
        pthread_join(srv->workers[i], NULL);
    return 0;
}

void http_server_stop(http_server_t *srv)
{
    if (!srv)
        return;
    srv->stop = 1;
    if (srv->listen_fd >= 0) {
        shutdown(srv->listen_fd, SHUT_RDWR);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}

int http_server_port(const http_server_t *srv)
{
    return srv ? srv->bound_port : 0;
}

void http_server_destroy(http_server_t *srv)
{
    if (!srv)
        return;
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    free(srv);
}
