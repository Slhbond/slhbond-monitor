/*
 * api.c - the JSON endpoints.
 *
 * Sampling is cached for cfg->cache_ms so that a dashboard polling every few
 * seconds, plus any number of curl clients, cannot turn into a /proc-reading
 * storm. `?refresh=1` forces a fresh sample.
 */
#include "api.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "json.h"
#include "log.h"
#include "version.h"

void api_context_init(api_context_t *ctx, const config_t *cfg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = cfg;
    pthread_mutex_init(&ctx->lock, NULL);
}

void api_context_destroy(api_context_t *ctx)
{
    pthread_mutex_destroy(&ctx->lock);
}

/**
 * Copy a consistent snapshot out of the cache, refreshing it when it is older
 * than cache_ms or when the client asked for it.
 */
static void api_snapshot(api_context_t *ctx, int force, slh_snapshot_t *out)
{
    int64_t now = slh_monotonic_ms();
    int     ttl = ctx->cfg->cache_ms;

    pthread_mutex_lock(&ctx->lock);
    if (force || ctx->collected_ms == 0 || now - ctx->collected_ms >= ttl) {
        sysinfo_collect(&ctx->cache.sys);
        netinfo_collect(&ctx->cache.net);
        cpustat_collect(&ctx->cache.cpu);
        gpu_collect(&ctx->cache.gpu);
        netstat_collect(&ctx->cache.network);
        npu_collect(&ctx->cache.npu);
        disk_collect(&ctx->cache.disks);
        ctx->collected_ms = now;
    }
    *out = ctx->cache;
    pthread_mutex_unlock(&ctx->lock);
}

static void json_begin(strbuf_t *sb)
{
    sb_reset(sb);
    sb_appendc(sb, '{');
}

static void json_end(http_response_t *resp, strbuf_t *sb, int status)
{
    sb_appendc(sb, '}');
    resp->status = status;
    http_response_set_json(resp, sb);
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

void api_handle_system(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    sysinfo_to_json(&sb, &snap.sys, &first);
    netinfo_to_json(&sb, &snap.net, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_cpu(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    cpustat_to_json(&sb, &snap.cpu, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_gpu(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    gpu_to_json(&sb, &snap.gpu, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_network(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    netstat_to_json(&sb, &snap.network, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_npu(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    npu_to_json(&sb, &snap.npu, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_disks(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;
    size_t         i;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    json_add_int(&sb, "count", (long long)snap.disks.count, &first);
    json_add_bool(&sb, "check_in_progress", snap.disks.check_in_progress, &first);
    json_add_str(&sb, "check_device",
                 snap.disks.check_device[0] ? snap.disks.check_device : NULL,
                 &first);

    json_key(&sb, "disks", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < snap.disks.count; i++) {
        int f = 1;

        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        disk_to_json(&sb, &snap.disks.disk[i], &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');

    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_disk_check(const http_request_t *req, http_response_t *resp, void *ctx)
{
    char  device[64];
    char  message[192];
    int   rc;
    strbuf_t sb;
    int   first = 1;

    SLH_UNUSED(ctx);

    if (!http_request_query_get(req, "device", device, sizeof(device)) ||
        device[0] == '\0') {
        http_response_set_json_error(resp, 400, "missing_device",
                                     "缺少 device 参数，例如 /api/v1/disk/check?device=sda");
        return;
    }
    if (!disk_device_exists(device)) {
        snprintf(message, sizeof(message), "未找到硬盘 %s", device);
        http_response_set_json_error(resp, 404, "unknown_device", message);
        return;
    }

    rc = disk_request_check(device);
    if (rc != 0) {
        http_response_set_json_error(resp, 503, "queue_full",
                                     "检测队列已满，请稍后重试");
        return;
    }

    sb_init(&sb);
    json_begin(&sb);
    json_add_bool(&sb, "accepted", 1, &first);
    json_add_str(&sb, "device", device, &first);
    json_add_str(&sb, "message", "已开始健康检测，请稍后刷新查看结果", &first);
    json_end(resp, &sb, 202);
    sb_free(&sb);
}

void api_handle_metrics(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    api_snapshot(ctx, http_request_query_flag(req, "refresh"), &snap);

    sb_init(&sb);
    json_begin(&sb);
    json_add_str(&sb, "hostname", snap.sys.hostname, &first);
    json_add_uint(&sb, "uptime_seconds", snap.sys.uptime_seconds, &first);
    /* CPU numbers come from cpustat so this endpoint cannot drift from the
     * CPU card on the dashboard. */
    json_add_int(&sb, "cpu_count", snap.cpu.threads, &first);
    if (snap.cpu.usage_percent < 0.0)
        json_add_null(&sb, "cpu_usage_percent", &first);
    else
        json_add_double(&sb, "cpu_usage_percent", snap.cpu.usage_percent, &first);
    sysinfo_metrics_to_json(&sb, &snap.sys, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_version(const http_request_t *req, http_response_t *resp, void *ctx)
{
    api_context_t  *api = ctx;
    version_state_t st;
    strbuf_t        sb;

    if (http_request_query_flag(req, "refresh"))
        version_check_now();

    version_state_get(&st);
    sb_init(&sb);
    version_to_json(&sb, &st, api->cfg);
    resp->status = 200;
    http_response_set_json(resp, &sb);
    sb_free(&sb);
}

void api_handle_health(const http_request_t *req, http_response_t *resp, void *ctx)
{
    slh_snapshot_t snap;
    strbuf_t       sb;
    int            first = 1;

    SLH_UNUSED(req);
    api_snapshot(ctx, 0, &snap);

    sb_init(&sb);
    json_begin(&sb);
    json_add_str(&sb, "status", "ok", &first);
    json_add_str(&sb, "version", SLH_VERSION_STRING, &first);
    json_add_uint(&sb, "uptime_seconds", snap.sys.uptime_seconds, &first);
    json_add_str(&sb, "hostname", snap.sys.hostname, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

void api_handle_status(const http_request_t *req, http_response_t *resp, void *ctx)
{
    api_context_t  *api = ctx;
    slh_snapshot_t  snap;
    version_state_t st;
    strbuf_t        sb;
    int             first;
    size_t          i;

    api_snapshot(api, http_request_query_flag(req, "refresh"), &snap);
    version_state_get(&st);

    sb_init(&sb);
    sb_appendc(&sb, '{');

    sb_append(&sb, "\"system\":{");
    first = 1;
    sysinfo_to_json(&sb, &snap.sys, &first);
    netinfo_to_json(&sb, &snap.net, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"cpu\":");
    first = 1;
    sb_appendc(&sb, '{');
    cpustat_to_json(&sb, &snap.cpu, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"gpu\":");
    first = 1;
    sb_appendc(&sb, '{');
    gpu_to_json(&sb, &snap.gpu, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"network\":");
    first = 1;
    sb_appendc(&sb, '{');
    netstat_to_json(&sb, &snap.network, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"npu\":");
    first = 1;
    sb_appendc(&sb, '{');
    npu_to_json(&sb, &snap.npu, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"storage\":{");
    first = 1;
    json_add_int(&sb, "count", (long long)snap.disks.count, &first);
    json_add_bool(&sb, "check_in_progress", snap.disks.check_in_progress, &first);
    json_add_str(&sb, "check_device",
                 snap.disks.check_device[0] ? snap.disks.check_device : NULL,
                 &first);
    json_key(&sb, "disks", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < snap.disks.count; i++) {
        int f = 1;

        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        disk_to_json(&sb, &snap.disks.disk[i], &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');
    sb_append(&sb, "},");

    sb_append(&sb, "\"metrics\":{");
    first = 1;
    sysinfo_metrics_to_json(&sb, &snap.sys, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"version\":");
    version_to_json(&sb, &st, api->cfg);
    sb_appendc(&sb, ',');

    first = 1;
    json_add_int(&sb, "poll_interval_ms", api->cfg->poll_ms, &first);
    json_add_uint(&sb, "server_uptime_seconds", snap.sys.uptime_seconds, &first);

    sb_appendc(&sb, '}');
    resp->status = 200;
    http_response_set_json(resp, &sb);
    sb_free(&sb);
}
