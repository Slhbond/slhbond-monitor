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
#define SLH_MANIFEST_DEFAULT    "/opt/slhbond-monitor/update.json"
/* Directory for small persistent runtime state. It must be writable by the
 * service account; under systemd this is provided by StateDirectory=, which
 * creates the path with the DynamicUser's ownership. */
#define SLH_STATE_DIR_DEFAULT   "/var/lib/slhbond-monitor"

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
    int  update_interval_sec;

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
