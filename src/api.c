/*
 * api.c - the JSON endpoints.
 *
 * Sampling is cached for cfg->cache_ms so that a dashboard polling every few
 * seconds, plus any number of curl clients, cannot turn into a /proc-reading
 * storm. `?refresh=1` forces a fresh sample.
 */
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alerts.h"
#include "cmdlog.h"
#include "events.h"
#include "httpclient.h"
#include "updater.h"
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
 * than `max_age_ms` (0 = the configured cache_ms).
 */
void api_snapshot_copy(api_context_t *ctx, int64_t max_age_ms, slh_snapshot_t *out)
{
    int64_t now = slh_monotonic_ms();
    int64_t ttl = max_age_ms > 0 ? max_age_ms : (int64_t)ctx->cfg->cache_ms;

    pthread_mutex_lock(&ctx->lock);
    if (ctx->collected_ms == 0 || now - ctx->collected_ms >= ttl) {
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

/** Handler-facing wrapper: `force` bypasses the cache entirely (?refresh=1). */
static void api_snapshot(api_context_t *ctx, int force, slh_snapshot_t *out)
{
    if (force) {
        pthread_mutex_lock(&ctx->lock);
        ctx->collected_ms = 0;
        pthread_mutex_unlock(&ctx->lock);
    }
    api_snapshot_copy(ctx, 0, out);
}

/**
 * Turn one live snapshot into a history record.
 *
 * Everything here is a *scale* decision, not a new measurement: the packed
 * record has one byte per percentage, so values are only clamped, never
 * recomputed. Metrics the kernel does not expose stay at -1 and are stored as
 * the "n/a" sentinel rather than as a zero that would read as "idle".
 */
int api_history_source(void *user, hist_sample_t *out)
{
    api_context_t *ctx = user;
    slh_snapshot_t snap;
    size_t         i;

    memset(out, 0, sizeof(*out));
    api_snapshot_copy(ctx, 1000, &snap);

    out->ts = slh_now_sec();
    out->cpu_avg = snap.cpu.usage_percent;
    out->cpu_max = snap.cpu.max_core_percent;
    out->temp_c = snap.cpu.temp_c;
    out->freq_mhz = snap.cpu.freq_cur_khz > 0
        ? (double)snap.cpu.freq_cur_khz / 1000.0 : 0.0;
    out->load1 = snap.sys.load1;

    out->mem_pct = snap.sys.mem_total_kb > 0
        ? 100.0 * (double)snap.sys.mem_used_kb / (double)snap.sys.mem_total_kb
        : -1.0;

    /* Primary interface by traffic, same choice the network card makes. */
    out->net_rx_bps = -1.0;
    out->net_tx_bps = -1.0;
    if (snap.network.primary >= 0 &&
        (size_t)snap.network.primary < snap.network.count) {
        const net_series_t *s = &snap.network.iface[snap.network.primary].s;

        out->net_rx_bps = s->rx_bps;
        out->net_tx_bps = s->tx_bps;
    }

    /* Sum across disks: the card shows per-disk rates, the series keeps the
     * total so the trace view can still show one meaningful curve. */
    out->disk_r_bps = 0.0;
    out->disk_w_bps = 0.0;
    out->disk_temp_c = -1.0;
    for (i = 0; i < snap.disks.count; i++) {
        double t = snap.disks.disk[i].temperature_c;

        out->disk_r_bps += snap.disks.disk[i].read_bps;
        out->disk_w_bps += snap.disks.disk[i].write_bps;
        /* The hottest drive is the one worth alarming on. */
        if (t >= 0.0 && t > out->disk_temp_c)
            out->disk_temp_c = t;
    }

    /* panfrost reports no utilisation; the power-on share is the honest
     * stand-in and is what the card shows too. */
    out->gpu_pct = snap.gpu.present ? snap.gpu.active_ratio_percent : -1.0;
    out->gpu_temp_c = snap.gpu.present ? snap.gpu.temp_c : -1.0;
    out->gpu_freq_mhz = snap.gpu.present
        ? (double)snap.gpu.freq_cur_hz / 1000000.0 : 0.0;
    out->npu_pct = snap.npu.load_available ? snap.npu.load_percent : -1.0;

    out->ncores = (int)snap.cpu.core_count;
    if (out->ncores > SLH_HIST_MAX_CORES)
        out->ncores = SLH_HIST_MAX_CORES;
    for (i = 0; i < (size_t)out->ncores; i++)
        out->core[i] = snap.cpu.core[i].usage_percent;

    return 0;
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

/* ------------------------------------------------------------------ */
/* History / alerts / events                                           */
/* ------------------------------------------------------------------ */

/**
 * One row of the alert metric table: the single place that knows how a metric
 * is named, which query parameter sets it, and what range is sane. Both the
 * read endpoint and the write endpoint are driven from it.
 */
typedef struct {
    const char *key;      /* query parameter name, e.g. "disk_temp"        */
    const char *name;     /* human label                                   */
    const char *unit;
    double      lo;
    double      hi;
    double      threshold;
} api_alert_metric_def_t;

static void api_alert_metric_def(int metric, api_alert_metric_def_t *def,
                                 const alert_thresholds_t *lim)
{
    memset(def, 0, sizeof(*def));
    switch (metric) {
    case ALERT_METRIC_CPU:
        def->key = "cpu"; def->name = alerts_metric_name(metric);
        def->unit = "%"; def->lo = 1.0; def->hi = 100.0;
        def->threshold = lim->cpu_percent;
        break;
    case ALERT_METRIC_MEM:
        def->key = "mem"; def->name = alerts_metric_name(metric);
        def->unit = "%"; def->lo = 1.0; def->hi = 100.0;
        def->threshold = lim->mem_percent;
        break;
    case ALERT_METRIC_TEMP:
        def->key = "temp"; def->name = alerts_metric_name(metric);
        def->unit = "°C"; def->lo = 20.0; def->hi = 150.0;
        def->threshold = lim->temp_c;
        break;
    case ALERT_METRIC_GPU:
        def->key = "gpu"; def->name = alerts_metric_name(metric);
        def->unit = "%"; def->lo = 1.0; def->hi = 100.0;
        def->threshold = lim->gpu_percent;
        break;
    case ALERT_METRIC_NPU:
        def->key = "npu"; def->name = alerts_metric_name(metric);
        def->unit = "%"; def->lo = 1.0; def->hi = 100.0;
        def->threshold = lim->npu_percent;
        break;
    case ALERT_METRIC_DISK_TEMP:
        def->key = "disk_temp"; def->name = alerts_metric_name(metric);
        def->unit = "°C"; def->lo = 20.0; def->hi = 100.0;
        def->threshold = lim->disk_temp_c;
        break;
    default:
        def->key = "?"; def->name = "未知"; def->unit = "";
        def->lo = 0.0; def->hi = 0.0; def->threshold = 0.0;
        break;
    }
}

static void alert_metric_apply(alert_thresholds_t *lim, int metric, double v)
{
    switch (metric) {
    case ALERT_METRIC_CPU:       lim->cpu_percent = v; break;
    case ALERT_METRIC_MEM:       lim->mem_percent = v; break;
    case ALERT_METRIC_TEMP:      lim->temp_c = v; break;
    case ALERT_METRIC_GPU:       lim->gpu_percent = v; break;
    case ALERT_METRIC_NPU:       lim->npu_percent = v; break;
    case ALERT_METRIC_DISK_TEMP: lim->disk_temp_c = v; break;
    default: break;
    }
}

/** Shared ?from= / ?to= / ?limit= parsing with generous defaults. */
static int64_t query_i64(const http_request_t *req, const char *name,
                         int64_t fallback)
{
    char buf[32];

    if (!http_request_query_get(req, name, buf, sizeof(buf)) || buf[0] == '\0')
        return fallback;
    return (int64_t)strtoll(buf, NULL, 10);
}

/**
 * GET /api/v1/history?from=&to=&limit=
 *
 * Records are emitted as positional arrays rather than objects: a day of
 * samples is 86400 rows, and repeating the field names would multiply the
 * response size for no benefit. The order is fixed and documented in the
 * README and in web/app.js's HIST_FIELDS.
 */
void api_handle_history(const http_request_t *req, http_response_t *resp, void *ctx)
{
    api_context_t *api = ctx;
    hist_record_t *recs;
    strbuf_t       sb;
    int            first = 1;
    int            decimated = 0;
    int            ncores = 0;
    int64_t        from;
    int64_t        to;
    int64_t        limit;
    int64_t        range_from;
    int64_t        range_to;
    size_t         stored = 0;
    size_t         capacity = 0;
    int            interval = 1;
    size_t         got;
    size_t         i;

    from = query_i64(req, "from", 0);
    to = query_i64(req, "to", 0);
    limit = query_i64(req, "limit", 2000);
    if (limit < 1)
        limit = 1;
    if (limit > 20000)
        limit = 20000;

    recs = xcalloc((size_t)limit, sizeof(*recs));
    got = history_query(from, to, recs, (size_t)limit, &decimated);
    history_stats(&stored, &capacity, &interval);
    history_range(&range_from, &range_to);

    for (i = 0; i < got; i++) {
        if (recs[i].ncores > ncores)
            ncores = recs[i].ncores;
    }

    sb_init(&sb);
    json_begin(&sb);
    json_add_int(&sb, "interval_sec", interval, &first);
    json_add_int(&sb, "retention_hours", api->cfg->history_hours, &first);
    json_add_uint(&sb, "capacity", (uint64_t)capacity, &first);
    json_add_uint(&sb, "stored", (uint64_t)stored, &first);
    json_add_uint(&sb, "disk_bytes", history_disk_bytes(), &first);
    json_add_bool(&sb, "decimated", decimated, &first);
    json_add_int(&sb, "ncores", ncores, &first);
    if (range_from)
        json_add_int(&sb, "oldest", range_from, &first);
    else
        json_add_null(&sb, "oldest", &first);
    if (range_to)
        json_add_int(&sb, "newest", range_to, &first);
    else
        json_add_null(&sb, "newest", &first);
    json_add_uint(&sb, "count", (uint64_t)got, &first);

    json_key(&sb, "records", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < got; i++) {
        const hist_record_t *r = &recs[i];
        int c;

        if (i)
            sb_appendc(&sb, ',');
        /* ts, cpu, cpuMax, mem, tempC10, freqMHz, load1x100,
         * netRx, netTx, diskR, diskW, gpu, npu, gpuTempC10, gpuFreqMHz,
         * diskTempC10, then one entry per core. */
        sb_appendf(&sb, "[%lld,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d,%d,%d,%d",
                   (long long)r->ts, r->cpu_avg, r->cpu_max, r->mem_pct,
                   r->temp_c10, r->freq_mhz, r->load1_x100,
                   r->net_rx_bps, r->net_tx_bps, r->disk_r_bps, r->disk_w_bps,
                   r->gpu_pct, r->npu_pct, r->gpu_temp_c10, r->gpu_freq_mhz,
                   r->disk_temp_c10);
        for (c = 0; c < r->ncores; c++)
            sb_appendf(&sb, ",%d", r->core[c]);
        sb_appendc(&sb, ']');
    }
    sb_appendc(&sb, ']');

    json_end(resp, &sb, 200);
    sb_free(&sb);
    free(recs);
}

/** GET /api/v1/alerts?limit= */
void api_handle_alerts(const http_request_t *req, http_response_t *resp, void *ctx)
{
    alert_record_t   *recs;
    alert_thresholds_t lim;
    strbuf_t          sb;
    int               first = 1;
    int64_t           limit;
    size_t            got;
    size_t            i;

    SLH_UNUSED(ctx);

    limit = query_i64(req, "limit", SLH_MAX_ALERTS);
    if (limit < 1)
        limit = 1;
    if (limit > SLH_MAX_ALERTS)
        limit = SLH_MAX_ALERTS;

    recs = xcalloc((size_t)limit, sizeof(*recs));
    got = alerts_list(recs, (size_t)limit);
    alerts_get_thresholds(&lim);

    sb_init(&sb);
    json_begin(&sb);
    json_add_uint(&sb, "total", (uint64_t)alerts_total(), &first);
    json_add_int(&sb, "active", alerts_active_count(), &first);
    json_add_uint(&sb, "disk_bytes", alerts_disk_bytes(), &first);

    json_key(&sb, "thresholds", &first);
    sb_appendc(&sb, '{');
    {
        int f = 1;

        json_add_double(&sb, "cpu_percent", lim.cpu_percent, &f);
        json_add_double(&sb, "mem_percent", lim.mem_percent, &f);
        json_add_double(&sb, "temp_c", lim.temp_c, &f);
        json_add_double(&sb, "gpu_percent", lim.gpu_percent, &f);
        json_add_double(&sb, "npu_percent", lim.npu_percent, &f);
        json_add_double(&sb, "disk_temp_c", lim.disk_temp_c, &f);
        json_add_int(&sb, "hold_sec", lim.hold_sec, &f);
    }
    sb_appendc(&sb, '}');

    /*
     * Data-driven metric table. The panel renders whatever is listed here, so
     * adding a metric means touching this table and alerts.c only - not the
     * frontend. `available` says whether the board can actually measure it:
     * a metric that is permanently 0 must not read as "all fine".
     */
    json_key(&sb, "metrics", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < (size_t)ALERT_METRIC_COUNT; i++) {
        api_alert_metric_def_t def;
        int f = 1;

        api_alert_metric_def((int)i, &def, &lim);
        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        json_add_str(&sb, "key", def.key, &f);
        json_add_str(&sb, "name", def.name, &f);
        json_add_str(&sb, "unit", def.unit, &f);
        json_add_double(&sb, "threshold", def.threshold, &f);
        json_add_double(&sb, "min", def.lo, &f);
        json_add_double(&sb, "max", def.hi, &f);
        json_add_bool(&sb, "available", alerts_metric_available((int)i), &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');
    first = 0;

    json_key(&sb, "alerts", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < got; i++) {
        const alert_record_t *r = &recs[i];
        int f = 1;

        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        json_add_int(&sb, "ts", r->ts, &f);
        if (r->ended_ts)
            json_add_int(&sb, "ended_ts", r->ended_ts, &f);
        else
            json_add_null(&sb, "ended_ts", &f);
        json_add_str(&sb, "metric",
                     r->metric == ALERT_METRIC_CPU ? "cpu"
                     : (r->metric == ALERT_METRIC_MEM ? "mem" : "temp"), &f);
        json_add_str(&sb, "metric_name", alerts_metric_name(r->metric), &f);
        json_add_double(&sb, "peak", r->peak, &f);
        json_add_double(&sb, "threshold", r->threshold, &f);
        json_add_int(&sb, "duration_sec", r->duration_sec, &f);
        json_add_bool(&sb, "ongoing", r->ended_ts == 0, &f);
        json_add_bool(&sb, "interrupted", r->interrupted, &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');

    /*
     * Live per-metric state. Without this the panel can only say "nothing was
     * recorded", which looks like a bug whenever a spike was real but did not
     * last the full hold time.
     */
    json_key(&sb, "states", &first);
    sb_appendc(&sb, '[');
    {
        alert_metric_state_t st[ALERT_METRIC_COUNT];
        size_t               n = alerts_metric_states(st, ALERT_METRIC_COUNT);

        for (i = 0; i < n; i++) {
            api_alert_metric_def_t def;
            int                    f = 1;

            api_alert_metric_def(st[i].metric, &def, &lim);
            if (i)
                sb_appendc(&sb, ',');
            sb_appendc(&sb, '{');
            json_add_str(&sb, "key", def.key, &f);
            json_add_str(&sb, "name", def.name, &f);
            json_add_str(&sb, "unit", def.unit, &f);
            if (st[i].available)
                json_add_double(&sb, "current", st[i].current, &f);
            else
                json_add_null(&sb, "current", &f);
            json_add_bool(&sb, "available", st[i].available, &f);
            json_add_bool(&sb, "active", st[i].active, &f);
            json_add_int(&sb, "pending_sec", st[i].pending_sec, &f);
            json_add_double(&sb, "pending_peak", st[i].pending_peak, &f);
            json_add_int(&sb, "hold_sec", st[i].hold_sec, &f);
            sb_appendc(&sb, '}');
        }
    }
    sb_appendc(&sb, ']');

    json_end(resp, &sb, 200);
    sb_free(&sb);
    free(recs);
}

/** GET /api/v1/commands?limit=&deletes=1 */
void api_handle_commands(const http_request_t *req, http_response_t *resp, void *ctx)
{
    cmd_record_t *recs;
    strbuf_t      sb;
    char          why[160];
    int           first = 1;
    int64_t       limit;
    int           only_deletes;
    size_t        got;
    size_t        i;

    SLH_UNUSED(ctx);

    limit = query_i64(req, "limit", 200);
    if (limit < 1)
        limit = 1;
    if (limit > SLH_MAX_COMMANDS)
        limit = SLH_MAX_COMMANDS;
    only_deletes = http_request_query_flag(req, "deletes");

    recs = xcalloc((size_t)limit, sizeof(*recs));
    got = only_deletes ? cmdlog_list_deletes(recs, (size_t)limit)
                       : cmdlog_list(recs, (size_t)limit);

    why[0] = '\0';
    sb_init(&sb);
    json_begin(&sb);
    json_add_uint(&sb, "total", (uint64_t)cmdlog_total(), &first);
    json_add_uint(&sb, "deletes", (uint64_t)cmdlog_delete_count(), &first);
    json_add_uint(&sb, "disk_bytes", cmdlog_disk_bytes(), &first);
    json_add_bool(&sb, "deletes_only", only_deletes, &first);
    {
        int ok = cmdlog_history_status(why, sizeof(why)) == 0;

        json_add_bool(&sb, "history_tracked", ok, &first);
        json_add_str(&sb, "history_note", ok ? NULL : why, &first);
    }

    json_key(&sb, "commands", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < got; i++) {
        const cmd_record_t *r = &recs[i];
        int                 f = 1;

        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        json_add_int(&sb, "ts", r->ts, &f);
        json_add_str(&sb, "source",
                     r->source == CMD_SOURCE_HISTORY ? "history" : "proc", &f);
        json_add_str(&sb, "user", r->user[0] ? r->user : NULL, &f);
        json_add_int(&sb, "uid", r->uid, &f);
        json_add_int(&sb, "pid", r->pid, &f);
        json_add_bool(&sb, "is_delete", r->is_delete, &f);
        json_add_str(&sb, "command", r->cmd[0] ? r->cmd : NULL, &f);
        json_add_str(&sb, "target", r->target[0] ? r->target : NULL, &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');

    json_end(resp, &sb, 200);
    sb_free(&sb);
    free(recs);
}

/**
 * POST /api/v1/alerts/config?cpu=90&mem=90&temp=80&gpu=90&npu=90&disk_temp=55&hold=10
 *
 * Every parameter is optional; only the ones present are changed. The metric
 * table above drives both parsing and range checking.
 */
void api_handle_alert_config(const http_request_t *req, http_response_t *resp,
                             void *ctx)
{
    alert_thresholds_t cur;
    strbuf_t           sb;
    char               err[192];
    int                first = 1;
    char               buf[32];
    int                m;

    SLH_UNUSED(ctx);

    alerts_get_thresholds(&cur);

    for (m = 0; m < ALERT_METRIC_COUNT; m++) {
        api_alert_metric_def_t def;
        double                 v;

        api_alert_metric_def(m, &def, &cur);
        if (!http_request_query_get(req, def.key, buf, sizeof(buf)) || !buf[0])
            continue;

        v = atof(buf);
        if (v < def.lo || v > def.hi) {
            snprintf(err, sizeof(err), "%s 必须在 %.0f-%.0f 之间",
                     def.name, def.lo, def.hi);
            http_response_set_json_error(resp, 400, "bad_threshold", err);
            return;
        }
        alert_metric_apply(&cur, m, v);
    }

    if (http_request_query_get(req, "hold", buf, sizeof(buf)) && buf[0]) {
        int h = atoi(buf);

        if (h < 1 || h > 3600) {
            http_response_set_json_error(resp, 400, "bad_hold",
                                         "hold 必须在 1-3600 秒之间");
            return;
        }
        cur.hold_sec = h;
    }

    if (alerts_set_thresholds(&cur, err, sizeof(err)) != 0) {
        http_response_set_json_error(resp, 400, "bad_threshold", err);
        return;
    }

    sb_init(&sb);
    json_begin(&sb);
    json_add_bool(&sb, "saved", 1, &first);
    json_add_double(&sb, "cpu_percent", cur.cpu_percent, &first);
    json_add_double(&sb, "mem_percent", cur.mem_percent, &first);
    json_add_double(&sb, "temp_c", cur.temp_c, &first);
    json_add_double(&sb, "gpu_percent", cur.gpu_percent, &first);
    json_add_double(&sb, "npu_percent", cur.npu_percent, &first);
    json_add_double(&sb, "disk_temp_c", cur.disk_temp_c, &first);
    json_add_int(&sb, "hold_sec", cur.hold_sec, &first);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

/** GET /api/v1/events?limit=&service=1 */
void api_handle_events(const http_request_t *req, http_response_t *resp, void *ctx)
{
    event_record_t *recs;
    strbuf_t        sb;
    char            why[192];
    int             first = 1;
    int64_t         limit;
    int             include_service;
    size_t          got;
    size_t          i;

    SLH_UNUSED(ctx);

    limit = query_i64(req, "limit", SLH_MAX_EVENTS);
    if (limit < 1)
        limit = 1;
    if (limit > SLH_MAX_EVENTS)
        limit = SLH_MAX_EVENTS;
    /* Service restarts are hidden unless asked for: they are not power cycles
     * and showing them as such is exactly the confusion this endpoint avoids. */
    include_service = http_request_query_flag(req, "service");

    recs = xcalloc((size_t)limit, sizeof(*recs));
    got = events_list(recs, (size_t)limit, include_service);

    why[0] = '\0';
    sb_init(&sb);
    json_begin(&sb);
    json_add_uint(&sb, "total", (uint64_t)got, &first);
    json_add_int(&sb, "boot_time", events_boot_time(), &first);
    json_add_bool(&sb, "just_booted", events_is_new_boot(), &first);
    json_add_bool(&sb, "service_shown", include_service, &first);
    json_add_uint(&sb, "disk_bytes", events_disk_bytes(), &first);
    {
        int ok = events_login_status(why, sizeof(why)) == 0;

        json_add_bool(&sb, "login_tracked", ok, &first);
        json_add_str(&sb, "login_note", ok ? NULL : why, &first);
    }

    json_key(&sb, "events", &first);
    sb_appendc(&sb, '[');
    for (i = 0; i < got; i++) {
        const event_record_t *r = &recs[i];
        int f = 1;

        if (i)
            sb_appendc(&sb, ',');
        sb_appendc(&sb, '{');
        json_add_int(&sb, "ts", r->ts, &f);
        json_add_str(&sb, "type", events_type_name(r->type), &f);
        json_add_bool(&sb, "unclean", r->unclean, &f);
        json_add_str(&sb, "user", r->user[0] ? r->user : NULL, &f);
        json_add_str(&sb, "ip", r->ip[0] ? r->ip : NULL, &f);
        json_add_str(&sb, "detail", r->detail[0] ? r->detail : NULL, &f);
        sb_appendc(&sb, '}');
    }
    sb_appendc(&sb, ']');

    json_end(resp, &sb, 200);
    sb_free(&sb);
    free(recs);
}

/** Shared payload for /api/v1/update and the POST replies. */
static void update_to_json(strbuf_t *sb, int *first)
{
    updater_status_t      st;
    version_state_t       vs;
    char                  staged[700];

    updater_status_get(&st);
    version_state_get(&vs);
    updater_staged_path(staged, sizeof(staged));

    json_add_bool(sb, "can_apply",
                  vs.update_available && vs.release_url[0] != '\0', first);
    json_add_bool(sb, "staged", staged[0] != '\0', first);
    json_add_str(sb, "phase", updater_phase_name(st.phase), first);
    json_add_str(sb, "message", st.message[0] ? st.message : NULL, first);
    json_add_str(sb, "error", st.error[0] ? st.error : NULL, first);
    json_add_int(sb, "percent", st.percent, first);
    json_add_uint(sb, "bytes", st.bytes, first);
    if (st.version[0])
        json_add_str(sb, "version", st.version, first);
    else
        json_add_str(sb, "version",
                     vs.latest[0] ? vs.latest : NULL, first);
    json_add_bool(sb, "download_ready", http_https_tool(NULL, 0), first);
    if (staged[0])
        json_add_str(sb, "staged_path", staged, first);
    json_add_int(sb, "finished_at",
                 st.finished_at ? st.finished_at : 0, first);
}

/** GET /api/v1/update */
void api_handle_update(const http_request_t *req, http_response_t *resp, void *ctx)
{
    strbuf_t sb;
    int      first = 1;
    char     why[128];

    SLH_UNUSED(req);
    SLH_UNUSED(ctx);

    why[0] = '\0';
    sb_init(&sb);
    json_begin(&sb);
    update_to_json(&sb, &first);
    if (!http_https_tool(NULL, 0))
        json_add_str(&sb, "download_note",
                     "系统里没有 curl 或 wget，无法从 https 下载（GitHub 需要它）",
                     &first);
    SLH_UNUSED(why);
    json_end(resp, &sb, 200);
    sb_free(&sb);
}

/** POST /api/v1/update/check */
void api_handle_update_check(const http_request_t *req, http_response_t *resp,
                             void *ctx)
{
    strbuf_t sb;
    int      first = 1;
    int      rc;

    SLH_UNUSED(req);
    SLH_UNUSED(ctx);

    rc = version_check_now();

    sb_init(&sb);
    json_begin(&sb);
    json_add_bool(&sb, "checked", rc == 0, &first);
    update_to_json(&sb, &first);
    json_end(resp, &sb, rc == 0 ? 200 : 502);
    sb_free(&sb);
}

/**
 * POST /api/v1/update/apply
 *
 * Only downloads and verifies; installing needs root and is done by
 * slhbond-update.service, which watches for the staged release.
 */
void api_handle_update_apply(const http_request_t *req, http_response_t *resp,
                             void *ctx)
{
    strbuf_t sb;
    int      first = 1;
    char     err[256];

    SLH_UNUSED(req);
    SLH_UNUSED(ctx);

    err[0] = '\0';
    if (updater_apply_async(err, sizeof(err)) != 0) {
        http_response_set_json_error(resp, 409, "cannot_apply", err);
        return;
    }

    sb_init(&sb);
    json_begin(&sb);
    json_add_bool(&sb, "accepted", 1, &first);
    json_add_str(&sb, "message",
                 "已开始下载并校验；完成后由系统自动应用并重启服务", &first);
    update_to_json(&sb, &first);
    json_end(resp, &sb, 202);
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

    /*
     * The three blocks below carry *summaries* only. The full alert, event
     * and history payloads can be hundreds of kilobytes, and this endpoint is
     * polled every few seconds; the dashboard fetches the detail on demand
     * when a tile is opened.
     */
    sb_append(&sb, "\"alerts\":{");
    first = 1;
    json_add_uint(&sb, "total", (uint64_t)alerts_total(), &first);
    json_add_int(&sb, "active", alerts_active_count(), &first);
    json_add_uint(&sb, "disk_bytes", alerts_disk_bytes(), &first);
    json_add_int(&sb, "metric_count", (int)ALERT_METRIC_COUNT, &first);
    {
        alert_thresholds_t lim;
        int                f = 0;

        alerts_get_thresholds(&lim);
        json_add_double(&sb, "cpu_percent", lim.cpu_percent, &f);
        json_add_double(&sb, "mem_percent", lim.mem_percent, &f);
        json_add_double(&sb, "temp_c", lim.temp_c, &f);
        json_add_double(&sb, "gpu_percent", lim.gpu_percent, &f);
        json_add_double(&sb, "npu_percent", lim.npu_percent, &f);
        json_add_double(&sb, "disk_temp_c", lim.disk_temp_c, &f);
        json_add_int(&sb, "hold_sec", lim.hold_sec, &f);
    }
    sb_append(&sb, "},");

    sb_append(&sb, "\"events\":{");
    first = 1;
    json_add_int(&sb, "boot_time", events_boot_time(), &first);
    json_add_bool(&sb, "just_booted", events_is_new_boot(), &first);
    json_add_uint(&sb, "disk_bytes", events_disk_bytes(), &first);
    {
        char why[192];
        int  ok;

        why[0] = '\0';
        ok = events_login_status(why, sizeof(why)) == 0;
        json_add_bool(&sb, "login_tracked", ok, &first);
        json_add_str(&sb, "login_note", ok ? NULL : why, &first);
    }
    sb_append(&sb, "},");

    sb_append(&sb, "\"update\":{");
    first = 1;
    update_to_json(&sb, &first);
    sb_append(&sb, "},");

    sb_append(&sb, "\"commands\":{");    first = 1;
    json_add_uint(&sb, "total", (uint64_t)cmdlog_total(), &first);
    json_add_uint(&sb, "deletes", (uint64_t)cmdlog_delete_count(), &first);
    json_add_uint(&sb, "disk_bytes", cmdlog_disk_bytes(), &first);
    {
        char why[160];
        int  ok;

        why[0] = '\0';
        ok = cmdlog_history_status(why, sizeof(why)) == 0;
        json_add_bool(&sb, "history_tracked", ok, &first);
        json_add_str(&sb, "history_note", ok ? NULL : why, &first);
    }
    sb_append(&sb, "},");

    sb_append(&sb, "\"history\":{");    first = 1;
    {
        size_t  stored = 0;
        size_t  capacity = 0;
        int     interval = 1;
        int64_t hfrom = 0;
        int64_t hto = 0;

        history_stats(&stored, &capacity, &interval);
        history_range(&hfrom, &hto);
        json_add_uint(&sb, "stored", (uint64_t)stored, &first);
        json_add_uint(&sb, "capacity", (uint64_t)capacity, &first);
        json_add_int(&sb, "interval_sec", interval, &first);
        json_add_uint(&sb, "disk_bytes", history_disk_bytes(), &first);
        if (hfrom)
            json_add_int(&sb, "oldest", hfrom, &first);
        else
            json_add_null(&sb, "oldest", &first);
        if (hto)
            json_add_int(&sb, "newest", hto, &first);
        else
            json_add_null(&sb, "newest", &first);
    }
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
