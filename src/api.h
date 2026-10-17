/*
 * api.h - JSON endpoints served under /api/v1/.
 *
 * Endpoints
 *   GET  /api/v1/system      host identity + addresses (系统信息卡片)
 *   GET  /api/v1/cpu         CPU model, load, per-core bars, temperature
 *   GET  /api/v1/gpu         GPU model, frequency operating point, thermal
 *   GET  /api/v1/network     per-interface counters + throughput history
 *   GET  /api/v1/npu         NPU presence, driver state and load
 *   GET  /api/v1/disks       every block device with its health verdict
 *   POST /api/v1/disk/check  run a health check now (?device=sda)
 *   GET  /api/v1/metrics     load average / memory / disk
 *   GET  /api/v1/history     per-second hardware history (?from=&to=&limit=)
 *   GET  /api/v1/alerts      threshold breaches (?limit=)
 *   POST /api/v1/alerts/config  change the thresholds
 *   GET  /api/v1/events      boot / shutdown / login records (?limit=)
 *   GET  /api/v1/commands    executed commands, deletions flagged (?limit=&deletes=1)
 *   GET  /api/v1/version     build version and update availability
 *   GET  /api/v1/update      self-update state (staged? downloading?)
 *   POST /api/v1/update/check  poll the manifest now
 *   POST /api/v1/update/apply  download + verify + stage the release
 *   GET  /api/v1/health      liveness probe
 *   GET  /api/v1/status      everything above in one round trip (used by the UI)
 *
 * Every GET accepts ?refresh=1 to bypass the short-lived sample cache.
 */
#ifndef SLH_API_H
#define SLH_API_H

#include <pthread.h>

#include "config.h"
#include "cpustat.h"
#include "disk.h"
#include "gpu.h"
#include "history.h"
#include "http.h"
#include "netinfo.h"
#include "netstat.h"
#include "npu.h"
#include "sysinfo.h"

/**
 * One consistently-timed set of samples. Bundling them keeps a single
 * response self-consistent (e.g. the donut and the per-core bars always come
 * from the same /proc/stat reading).
 */
typedef struct {
    sysinfo_t   sys;
    netinfo_t   net;
    cpustat_t   cpu;
    gpu_t       gpu;
    netlist_t   network;
    npu_t       npu;
    disk_list_t disks;
} slh_snapshot_t;

typedef struct {
    const config_t *cfg;
    pthread_mutex_t lock;         /* guards the cache below */
    slh_snapshot_t  cache;
    int64_t         collected_ms; /* monotonic ms of the last collection */
} api_context_t;

void api_context_init(api_context_t *ctx, const config_t *cfg);
void api_context_destroy(api_context_t *ctx);

/**
 * Copy a consistent snapshot, re-sampling when the cache is older than
 * `max_age_ms` (0 means "use cfg->cache_ms").
 *
 * This is the *only* place samples are taken, which matters: cpustat derives
 * its percentages from /proc/stat deltas, so a second collector running
 * alongside this one would steal those deltas and make both views wrong.
 */
void api_snapshot_copy(api_context_t *ctx, int64_t max_age_ms, slh_snapshot_t *out);

/** hist_source_fn for the history sampler; lives here for the reason above. */
int api_history_source(void *user, hist_sample_t *out);

void api_handle_system(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_cpu(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_gpu(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_network(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_npu(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_disks(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_disk_check(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_metrics(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_history(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_alerts(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_alert_config(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_events(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_commands(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_version(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_update(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_update_check(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_update_apply(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_health(const http_request_t *req, http_response_t *resp, void *ctx);
void api_handle_status(const http_request_t *req, http_response_t *resp, void *ctx);

#endif /* SLH_API_H */
