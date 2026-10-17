/*
 * config.h - runtime configuration: defaults, config file, command line.
 *
 * Precedence (lowest to highest): built-in defaults < config file < CLI flags.
 */
#ifndef SLH_CONFIG_H
#define SLH_CONFIG_H

#include "log.h"

#define SLH_CONFIG_PATH_DEFAULT "/etc/slhbond-monitor.conf"
#define SLH_BIND_DEFAULT        "0.0.0.0"
#define SLH_PORT_DEFAULT        8090
#define SLH_DOCROOT_DEFAULT     "/opt/slhbond-monitor/web"
#define SLH_WORKERS_DEFAULT     4
#define SLH_POLL_MS_DEFAULT     5000
#define SLH_CACHE_MS_DEFAULT    900
#define SLH_UPDATE_SEC_DEFAULT  1800
/*
 * 默认清单地址直接指向本项目在 GitHub 上「最新一次 Release」的清单。
 *
 * 这样别人装完不用改任何配置就能收到后续更新 —— 这正是发布这个项目的意义。
 * 想关掉或换成自己服务器的地址，改 /etc/slhbond-monitor.conf 里的
 * update_manifest（或 update_check = false）即可。
 */
#define SLH_MANIFEST_DEFAULT \
    "https://github.com/Slhbond/slhbond-monitor/releases/latest/download/update.json"
/* Directory for small persistent runtime state. It must be writable by the
 * service account; under systemd this is provided by StateDirectory=, which
 * creates the path with the DynamicUser's ownership. */
#define SLH_STATE_DIR_DEFAULT   "/var/lib/slhbond-monitor"

/* Per-second hardware history: 24 h x 36 B = ~3 MB on a 4-core board. */
#define SLH_HISTORY_HOURS_DEFAULT  24
#define SLH_HISTORY_FLUSH_DEFAULT  60

/* Alert thresholds; a breach must persist for HOLD seconds to be recorded. */
#define SLH_ALERT_CPU_DEFAULT       90.0
#define SLH_ALERT_MEM_DEFAULT       90.0
#define SLH_ALERT_TEMP_DEFAULT      80.0
#define SLH_ALERT_GPU_DEFAULT       90.0
#define SLH_ALERT_NPU_DEFAULT       90.0
#define SLH_ALERT_DISK_TEMP_DEFAULT 55.0
#define SLH_ALERT_HOLD_DEFAULT      10

typedef struct {
    /* --- HTTP listener -------------------------------------------- */
    char bind_address[64];
    int  port;
    int  worker_threads;

    /* --- content --------------------------------------------------- */
    char doc_root[512];

    /* Writable directory for small persistent state (e.g. the record of
     * which disks already had their automatic first health check). */
    char state_dir[512];

    /* --- logging --------------------------------------------------- */
    char        log_file[512];
    log_level_t log_level;
    int         foreground;      /* 0 = detach and run as a daemon */

    /* --- dashboard behaviour --------------------------------------- */
    int poll_ms;                 /* advertised to the UI as a poll hint */
    int cache_ms;                /* sysinfo cache lifetime              */

    /* --- update check ---------------------------------------------- */
    int  update_enabled;
    char update_manifest[512];   /* local path or http:// URL, "" = off */
    /*
     * 按固定间隔自动检查的秒数，0 表示关闭（只靠每日定时检查或手动按钮）。
     *
     * 默认关闭：用户要的是「每天 0 点查一次」，而不是不停地轮询上游。
     * 想更及时就设成 1800 之类，两种检查取更早的那个。
     */
    int  update_interval_sec;
    /*
     * Hour of the day (0-23) for the daily automatic check, -1 to disable.
     * 0 means midnight, which is what the panel documents. Kept separate from
     * update_interval_sec because "check every 30 minutes" and "check at 00:00
     * every night" are different promises and users ask for both.
     */
    int  update_daily_hour;

    /* --- per-second history ---------------------------------------- */
    int history_enabled;
    int history_hours;           /* retention window, in hours         */
    int history_interval_sec;    /* seconds between records (1 =每秒)   */
    int history_flush_sec;       /* records staged before a batch write */
    int history_cores;           /* 0 = follow the detected core count  */

    /* --- alerts ----------------------------------------------------- */
    double alert_cpu_percent;
    double alert_mem_percent;
    double alert_temp_c;
    double alert_gpu_percent;
    double alert_npu_percent;
    double alert_disk_temp_c;
    int    alert_hold_sec;

    /* --- bookkeeping ------------------------------------------------ */
    char config_path[512];       /* where settings came from, for /api */
} config_t;
/** Populate every field with its built-in default. */
void config_defaults(config_t *cfg);

/**
 * Apply `key = value` settings from a file.
 * Returns 0 on success, -1 when the file cannot be read, and logs (but does
 * not fail on) individual unknown keys so a typo cannot take the daemon down.
 */
int config_load_file(config_t *cfg, const char *path);

/**
 * Apply command line flags. Returns:
 *    0  continue start-up
 *    1  the caller should exit successfully (--help / --version)
 *   -1  a usage error was reported
 */
int config_parse_args(config_t *cfg, int argc, char **argv);

/** Clamp values into valid ranges and fill derived defaults. */
void config_normalise(config_t *cfg);

void config_print_usage(const char *prog);

#endif /* SLH_CONFIG_H */
