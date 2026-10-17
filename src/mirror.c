/*
 * mirror.c - 更新加速节点（第三方 GitHub 代理）
 * 详见 mirror.h。
 */
#include "mirror.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpclient.h"
#include "log.h"
#include "util.h"

/* 探测时只取最前面的一小段，够测出连接与首字节时间即可 */
#define PROBE_RANGE "0-32767"

static void trim_prefix(char *s)
{
    size_t n;

    while (*s == ' ')
        memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/** 补上结尾的 '/'，让「前缀 + 完整URL」拼出来是对的 */
static void normalise_prefix(char *s, size_t size)
{
    size_t n = strlen(s);

    if (n == 0 || n + 1 >= size)
        return;
    if (s[n - 1] != '/') {
        s[n] = '/';
        s[n + 1] = '\0';
    }
}

int mirror_is_direct(const mirror_t *m)
{
    return m && m->prefix[0] == '\0';
}

int mirror_parse(const char *spec, mirror_t *out, int max)
{
    int   count = 0;
    const char *p = spec;

    if (!out || max < 1)
        return 0;

    /* 第 0 个永远是直连 */
    memset(&out[0], 0, sizeof(out[0]));
    count = 1;

    if (!spec)
        return count;

    while (*p && count < max) {
        char   token[SLH_MIRROR_PREFIX];
        size_t len = 0;

        /* 跳过分隔符 */
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';')
            p++;
        if (!*p)
            break;

        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' &&
               len + 1 < sizeof(token)) {
            token[len++] = *p++;
        }
        token[len] = '\0';
        trim_prefix(token);

        if (token[0] == '\0')
            continue;
        /* 只接受 http(s) 前缀，避免把人坑进奇怪的协议 */
        if (!slh_starts_with(token, "http://") &&
            !slh_starts_with(token, "https://")) {
            LOG_WARN("update: 忽略无法识别的加速节点 '%s'", token);
            continue;
        }
        normalise_prefix(token, sizeof(token));

        memset(&out[count], 0, sizeof(out[count]));
        slh_strlcpy(out[count].prefix, token, sizeof(out[count].prefix));
        out[count].latency_ms = -1;
        count++;
    }
    return count;
}

int mirror_probe(const char *url, int timeout_ms)
{
    char  tool[512];
    char  secs[16];
    char  out[64];
    char  range[32];
    int   rc;
    double total;

    if (!url || !*url)
        return -1;
    if (!http_https_tool(tool, sizeof(tool)))
        return -1;

    snprintf(secs, sizeof(secs), "%d",
             (timeout_ms > 0 ? timeout_ms : 5000) / 1000 + 1);
    snprintf(range, sizeof(range), PROBE_RANGE);

    /*
     * 只取前 32 KB 并输出 time_total，用「能不能在一小段数据上拿到首字节」
     * 来代表整体延迟。比 HEAD 可靠：有些代理不支持 HEAD，还有些对 HEAD
     * 直接返回 200 但其实并不代理内容。
     */
    if (strstr(tool, "wget")) {
        const char *argv[] = { tool, "-q", "-T", secs, "-O", "/dev/null", url, NULL };

        rc = slh_run_command(argv, out, sizeof(out), timeout_ms);
    } else {
        const char *argv[] = { tool, "-sL", "--max-time", secs, "-r", range,
                               "-o", "/dev/null", "-w", "%{time_total}", url, NULL };

        rc = slh_run_command(argv, out, sizeof(out), timeout_ms + 3000);
    }
    if (rc != 0)
        return -1;

    /* wget 拿不到 time_total：能成功就算「够快」，用一个象征值 */
    if (strstr(tool, "wget"))
        return 1;

    total = strtod(out, NULL);
    if (total <= 0.0)
        return -1;
    if (total > 600.0)               /* 明显不对，按失败处理 */
        return -1;
    return (int)(total * 1000.0);
}

int mirror_pick(const char *url, mirror_t *cands, int count,
                int probe_ms, int good_enough_ms,
                char *out_url, size_t out_size,
                int *out_latency_ms, int *out_index)
{
    char cand_url[640];
    int  best = -1;
    int  best_ms = -1;
    int  i;

    if (!url || !*url || !cands || count < 1 || !out_url || out_size == 0)
        return -1;

    out_url[0] = '\0';
    if (out_latency_ms)
        *out_latency_ms = -1;
    if (out_index)
        *out_index = -1;

    for (i = 0; i < count; i++) {
        int ms;

        snprintf(cand_url, sizeof(cand_url), "%s%s", cands[i].prefix, url);
        ms = mirror_probe(cand_url, probe_ms);
        cands[i].latency_ms = ms;

        if (ms < 0)
            continue;
        if (best < 0 || ms < best_ms) {
            best = i;
            best_ms = ms;
            slh_strlcpy(out_url, cand_url, out_size);
        }
        /* 已经足够快就不用继续测了，省得用户点一下等五秒 */
        if (best_ms <= good_enough_ms)
            break;
    }

    if (best < 0)
        return -1;
    if (out_latency_ms)
        *out_latency_ms = best_ms;
    if (out_index)
        *out_index = best;
    return 0;
}

int mirror_repo_prefix(const char *manifest_url, char *out, size_t out_size)
{
    const char *p;
    const char *release;

    if (!manifest_url || !out || out_size == 0)
        return 0;
    out[0] = '\0';

    if (!slh_starts_with(manifest_url, "https://github.com/") &&
        !slh_starts_with(manifest_url, "http://github.com/"))
        return 0;

    /* 找到 /releases/ 的位置，截到它为止 */
    release = strstr(manifest_url, "/releases/");
    if (!release)
        return 0;
    p = release + strlen("/releases/");
    (void)p;

    {
        size_t len = (size_t)(release - manifest_url) + strlen("/releases/");

        if (len >= out_size)
            return 0;
        memcpy(out, manifest_url, len);
        out[len] = '\0';
    }
    return 1;
}
