/*
 * httpclient.c - just enough HTTP to fetch a small manifest file.
 */
#include "httpclient.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define MAX_BODY (256u * 1024u)
#define MAX_HEADER (32u * 1024u)

typedef struct {
    char scheme[16];
    char host[256];
    char port[16];
    char path[1024];
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
    const char *p = strstr(url, "://");
    const char *host_start;
    const char *path_start;

    memset(out, 0, sizeof(*out));
    if (!p) {
        slh_strlcpy(out->scheme, "http", sizeof(out->scheme));
        host_start = url;
    } else {
        size_t scheme_len = (size_t)(p - url);
        if (scheme_len >= sizeof(out->scheme))
            return -1;
        memcpy(out->scheme, url, scheme_len);
        out->scheme[scheme_len] = '\0';
        host_start = p + 3;
    }
    if (strcmp(out->scheme, "http") != 0)
        return -1;

    path_start = strchr(host_start, '/');
    if (!path_start)
        path_start = host_start + strlen(host_start);

    /* host[:port] */
    {
        size_t host_len = (size_t)(path_start - host_start);
        char   authority[300];
        char  *colon;

        if (host_len >= sizeof(authority))
            return -1;
        memcpy(authority, host_start, host_len);
        authority[host_len] = '\0';

        colon = strrchr(authority, ':');
        if (colon) {
            slh_strlcpy(out->port, colon + 1, sizeof(out->port));
            *colon = '\0';
        }
        slh_strlcpy(out->host, authority, sizeof(out->host));
    }
    if (out->host[0] == '\0')
        return -1;
    if (out->port[0] == '\0')
        slh_strlcpy(out->port, "80", sizeof(out->port));

    if (*path_start == '\0')
        slh_strlcpy(out->path, "/", sizeof(out->path));
    else
        slh_strlcpy(out->path, path_start, sizeof(out->path));
    return 0;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0)
        return rc;                                     /* 0 = timeout, -1 = error */
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        /* POLLHUP alongside readable data is fine; let the caller read it. */
        if (!(pfd.revents & events))
            return -1;
    }
    return 1;
}

static int connect_host(const char *host, const char *port, int timeout_ms,
                        char *err, size_t err_size)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, err_size, "无法解析主机 %.128s: %.96s", host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        int fd;
        int flags;
        int connected = 0;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            connected = 1;
        } else if (errno == EINPROGRESS) {
            if (wait_fd(fd, POLLOUT, timeout_ms) == 1) {
                int so_err = 0;
                socklen_t len = sizeof(so_err);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 &&
                    so_err == 0)
                    connected = 1;
            }
        }

        if (connected) {
            fcntl(fd, F_SETFL, flags);
            freeaddrinfo(res);
            return fd;
        }
        snprintf(err, err_size, "连接 %.128s:%.8s 失败: %.64s", host, port,
                 strerror(errno));
        close(fd);
    }

    freeaddrinfo(res);
    if (err[0] == '\0')
        snprintf(err, err_size, "无法连接 %.128s:%.8s", host, port);
    return -1;
}

static int send_all(int fd, const char *data, size_t len, int timeout_ms)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n;

        if (wait_fd(fd, POLLOUT, timeout_ms) != 1)
            return -1;
        n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/** Remove a chunked transfer-encoding wrapper in place. Returns 0 on success. */
static int dechunk(char *body, size_t *len)
{
    char  *src = body;
    char  *dst = body;
    size_t remaining = *len;

    while (remaining > 0) {
        char *end = NULL;
        unsigned long chunk = strtoul(src, &end, 16);
        size_t line_len;

        if (!end || end == src)
            return -1;
        line_len = (size_t)(end - src);
        if (line_len + 2 > remaining)
            return -1;
        src += line_len + 2;             /* skip "\r\n" */
        remaining -= line_len + 2;

        if (chunk == 0)
            break;
        if (chunk > remaining)
            return -1;
        memmove(dst, src, chunk);
        dst += chunk;
        src += chunk;
        remaining -= chunk;

        if (remaining >= 2) {            /* skip the trailing CRLF */
            src += 2;
            remaining -= 2;
        }
    }
    *len = (size_t)(dst - body);
    *dst = '\0';
    return 0;
}

int http_get(const char *url, http_client_response_t *resp, int timeout_ms)
{
    parsed_url_t u;
    char          err[512];
    char          request[2048];
    strbuf_t      raw;
    int           fd = -1;
    int           header_end = -1;
    long          content_length = -1;
    int           chunked = 0;
    int           result = -1;

    memset(resp, 0, sizeof(*resp));
    err[0] = '\0';

    if (parse_url(url, &u) != 0) {
        if (slh_starts_with(url, "https://"))
            snprintf(resp->error, sizeof(resp->error),
                     "不支持 https（无 TLS 依赖），请改用 http:// 或本地文件路径");
        else
            snprintf(resp->error, sizeof(resp->error), "无法解析 URL: %s", url);
        return -1;
    }

    fd = connect_host(u.host, u.port, timeout_ms, err, sizeof(err));
    if (fd < 0) {
        slh_strlcpy(resp->error, err, sizeof(resp->error));
        return -1;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%s\r\n"
             "User-Agent: Slhbond-Monitor/" SLH_VERSION_STRING "\r\n"
             "Accept: application/json, text/plain, */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             u.path, u.host, u.port);

    if (send_all(fd, request, strlen(request), timeout_ms) != 0) {
        slh_strlcpy(resp->error, "发送请求失败", sizeof(resp->error));
        close(fd);
        return -1;
    }

    sb_init(&raw);
    for (;;) {
        char    buf[4096];
        ssize_t n;

        if (raw.len > MAX_BODY + MAX_HEADER) {
            slh_strlcpy(resp->error, "响应过大", sizeof(resp->error));
            goto done;
        }
        if (wait_fd(fd, POLLIN, timeout_ms) != 1)
            break;
        n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (n == 0)
            break;                                   /* server closed */
        sb_appendn(&raw, buf, (size_t)n);

        if (header_end < 0) {
            char *sep = strstr(raw.data, "\r\n\r\n");
            if (sep) {
                char *line_end;
                char *line;

                header_end = (int)(sep - raw.data) + 4;

                /* status line */
                resp->status = 0;
                line = raw.data;
                line_end = strstr(line, "\r\n");
                if (line_end) {
                    char status_line[128];
                    size_t len = (size_t)(line_end - line);
                    if (len >= sizeof(status_line))
                        len = sizeof(status_line) - 1;
                    memcpy(status_line, line, len);
                    status_line[len] = '\0';
                    {
                        char *sp = strchr(status_line, ' ');
                        if (sp)
                            resp->status = atoi(sp + 1);
                    }
                }

                /* headers we care about */
                for (line = line_end ? line_end + 2 : NULL; line && line < sep; ) {
                    char *next = strstr(line, "\r\n");
                    size_t len = next ? (size_t)(next - line) : strlen(line);
                    char   header[512];
                    char  *colon;

                    if (len >= sizeof(header))
                        len = sizeof(header) - 1;
                    memcpy(header, line, len);
                    header[len] = '\0';
                    colon = strchr(header, ':');
                    if (colon) {
                        char *name = header;
                        char *value;
                        *colon = '\0';
                        value = slh_trim(colon + 1);
                        name = slh_trim(name);
                        if (slh_strcasecmp(name, "Content-Length") == 0)
                            content_length = strtol(value, NULL, 10);
                        else if (slh_strcasecmp(name, "Transfer-Encoding") == 0 &&
                                 strstr(value, "chunked"))
                            chunked = 1;
                        else if (slh_strcasecmp(name, "Content-Type") == 0)
                            slh_strlcpy(resp->content_type, value,
                                        sizeof(resp->content_type));
                    }
                    line = next ? next + 2 : NULL;
                }
            }
        }

        if (header_end > 0 && !chunked && content_length >= 0) {
            if ((size_t)(raw.len - (size_t)header_end) >= (size_t)content_length)
                break;                               /* body complete */
        }
    }

    if (header_end <= 0) {
        slh_strlcpy(resp->error, "响应格式无效", sizeof(resp->error));
        goto done;
    }

    {
        size_t body_len = raw.len - (size_t)header_end;
        char  *body = xmalloc(body_len + 1);

        memcpy(body, raw.data + header_end, body_len);
        body[body_len] = '\0';

        if (chunked) {
            if (dechunk(body, &body_len) != 0)
                LOG_WARN("malformed chunked response from %s", url);
        } else if (content_length >= 0 && body_len > (size_t)content_length) {
            body_len = (size_t)content_length;
            body[body_len] = '\0';
        }

        if (body_len > MAX_BODY) {
            body_len = MAX_BODY;
            body[MAX_BODY] = '\0';
        }
        resp->body = body;
        resp->body_len = body_len;
    }
    result = 0;

done:
    sb_free(&raw);
    close(fd);
    return result;
}

void http_client_response_free(http_client_response_t *resp)
{
    if (!resp)
        return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
