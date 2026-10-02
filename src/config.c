/*
 * config.c - config file and command line handling.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/* Defaults and validation                                             */
/* ------------------------------------------------------------------ */

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    slh_strlcpy(cfg->bind_address, SLH_BIND_DEFAULT, sizeof(cfg->bind_address));
    cfg->port = SLH_PORT_DEFAULT;
    cfg->worker_threads = SLH_WORKERS_DEFAULT;
    slh_strlcpy(cfg->doc_root, SLH_DOCROOT_DEFAULT, sizeof(cfg->doc_root));
    slh_strlcpy(cfg->state_dir, SLH_STATE_DIR_DEFAULT, sizeof(cfg->state_dir));
    cfg->log_file[0] = '\0';
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->foreground = 0;
    cfg->poll_ms = SLH_POLL_MS_DEFAULT;
    cfg->cache_ms = SLH_CACHE_MS_DEFAULT;
    cfg->update_enabled = 1;
    slh_strlcpy(cfg->update_manifest, SLH_MANIFEST_DEFAULT,
                sizeof(cfg->update_manifest));
    cfg->update_interval_sec = SLH_UPDATE_SEC_DEFAULT;
    cfg->config_path[0] = '\0';
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

void config_normalise(config_t *cfg)
{
    cfg->port = clamp_int(cfg->port, 1, 65535);
    cfg->worker_threads = clamp_int(cfg->worker_threads, 1, 64);
    cfg->poll_ms = clamp_int(cfg->poll_ms, 1000, 600000);
    cfg->cache_ms = clamp_int(cfg->cache_ms, 0, 60000);
    cfg->update_interval_sec = clamp_int(cfg->update_interval_sec, 60, 86400);

    if (cfg->bind_address[0] == '\0')
        slh_strlcpy(cfg->bind_address, SLH_BIND_DEFAULT, sizeof(cfg->bind_address));
    if (cfg->doc_root[0] == '\0')
        slh_strlcpy(cfg->doc_root, SLH_DOCROOT_DEFAULT, sizeof(cfg->doc_root));
    if (cfg->state_dir[0] == '\0')
        slh_strlcpy(cfg->state_dir, SLH_STATE_DIR_DEFAULT, sizeof(cfg->state_dir));
    if (cfg->update_manifest[0] == '\0')
        cfg->update_enabled = 0;
}

/* ------------------------------------------------------------------ */
/* Config file                                                         */
/* ------------------------------------------------------------------ */

static int parse_bool(const char *value, int *out)
{
    if (slh_strcasecmp(value, "1") == 0 || slh_strcasecmp(value, "true") == 0 ||
        slh_strcasecmp(value, "yes") == 0 || slh_strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (slh_strcasecmp(value, "0") == 0 || slh_strcasecmp(value, "false") == 0 ||
        slh_strcasecmp(value, "no") == 0 || slh_strcasecmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/**
 * Apply a single key/value pair.
 * Returns 1 when the key was recognised, 0 when it was not.
 */
static int apply_setting(config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "bind") == 0 || strcmp(key, "bind_address") == 0) {
        slh_strlcpy(cfg->bind_address, value, sizeof(cfg->bind_address));
        return 1;
    }
    if (strcmp(key, "port") == 0) {
        cfg->port = atoi(value);
        return 1;
    }
    if (strcmp(key, "workers") == 0 || strcmp(key, "threads") == 0) {
        cfg->worker_threads = atoi(value);
        return 1;
    }
    if (strcmp(key, "doc_root") == 0 || strcmp(key, "web_root") == 0) {
        slh_strlcpy(cfg->doc_root, value, sizeof(cfg->doc_root));
        return 1;
    }
    if (strcmp(key, "state_dir") == 0) {
        slh_strlcpy(cfg->state_dir, value, sizeof(cfg->state_dir));
        return 1;
    }
    if (strcmp(key, "log_file") == 0) {
        slh_strlcpy(cfg->log_file, value, sizeof(cfg->log_file));
        return 1;
    }
    if (strcmp(key, "log_level") == 0) {
        log_level_t level;
        if (log_level_parse(value, &level) == 0)
            cfg->log_level = level;
        return 1;
    }
    if (strcmp(key, "foreground") == 0) {
        parse_bool(value, &cfg->foreground);
        return 1;
    }
    if (strcmp(key, "poll_ms") == 0) {
        cfg->poll_ms = atoi(value);
        return 1;
    }
    if (strcmp(key, "cache_ms") == 0) {
        cfg->cache_ms = atoi(value);
        return 1;
    }
    if (strcmp(key, "update_check") == 0) {
        parse_bool(value, &cfg->update_enabled);
        return 1;
    }
    if (strcmp(key, "update_manifest") == 0) {
        slh_strlcpy(cfg->update_manifest, value, sizeof(cfg->update_manifest));
        cfg->update_enabled = value[0] != '\0';
        return 1;
    }
    if (strcmp(key, "update_interval") == 0) {
        cfg->update_interval_sec = atoi(value);
        return 1;
    }
    return 0;
}

int config_load_file(config_t *cfg, const char *path)
{
    FILE *fp;
    char  line[1024];
    int   lineno = 0;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *text;
        char *eq;

        lineno++;
        text = slh_trim(line);
        if (*text == '\0' || *text == '#' || *text == ';')
            continue;
        eq = strchr(text, '=');
        if (!eq) {
            LOG_WARN("%s:%d: ignoring line without '='", path, lineno);
            continue;
        }
        *eq = '\0';
        if (!apply_setting(cfg, slh_trim(text), slh_trim(eq + 1)))
            LOG_WARN("%s:%d: unknown setting '%s'", path, lineno, text);
    }
    fclose(fp);
    slh_strlcpy(cfg->config_path, path, sizeof(cfg->config_path));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

void config_print_usage(const char *prog)
{
    printf(
        "Slhbond-Monitor %s - lightweight Linux system monitor\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "  -c, --config <path>      config file (default: %s if present)\n"
        "  -b, --bind <address>     listen address (default %s)\n"
        "  -p, --port <port>        listen port (default %d)\n"
        "  -r, --docroot <path>     web asset directory (default %s)\n"
        "      --state-dir <path>   writable dir for small runtime state\n"
        "                           (default %s)\n"
        "  -t, --threads <n>        worker threads (default %d)\n"
        "  -l, --log-file <path>    also append logs to this file\n"
        "  -L, --log-level <level>  error | warn | info | debug\n"
        "  -f, --foreground         do not detach; log to stderr\n"
        "      --update-manifest <path|url>  release manifest, empty disables\n"
        "      --update-interval <sec>       update poll interval\n"
        "      --no-update-check             disable update checking\n"
        "  -V, --version            print version and exit\n"
        "  -h, --help               print this help and exit\n"
        "\n"
        "Config file format: one 'key = value' per line, '#' starts a comment.\n",
        SLH_VERSION_STRING, prog, SLH_CONFIG_PATH_DEFAULT,
        SLH_BIND_DEFAULT, SLH_PORT_DEFAULT, SLH_DOCROOT_DEFAULT,
        SLH_STATE_DIR_DEFAULT, SLH_WORKERS_DEFAULT);
}

/** Read the value of an option, either "--opt value" or "--opt=value". */
static const char *option_value(int argc, char **argv, int *index, const char *inline_value)
{
    if (inline_value)
        return inline_value;
    if (*index + 1 >= argc)
        return NULL;
    (*index)++;
    return argv[*index];
}

int config_parse_args(config_t *cfg, int argc, char **argv)
{
    const char *prog = argc > 0 ? argv[0] : "slhbond-monitor";
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *inline_value = NULL;
        const char *name = arg;
        char        name_buf[128];
        const char *value;

        if (arg[0] != '-' || arg[1] == '\0') {
            fprintf(stderr, "%s: unexpected argument '%s'\n", prog, arg);
            return -1;
        }
        if (slh_starts_with(arg, "--")) {
            const char *eq = strchr(arg, '=');
            if (eq) {
                size_t len = (size_t)(eq - arg);
                if (len >= sizeof(name_buf))
                    len = sizeof(name_buf) - 1;
                memcpy(name_buf, arg, len);
                name_buf[len] = '\0';
                name = name_buf;
                inline_value = eq + 1;
            }
        }

        if (strcmp(name, "-h") == 0 || strcmp(name, "--help") == 0) {
            config_print_usage(prog);
            return 1;
        }
        if (strcmp(name, "-V") == 0 || strcmp(name, "--version") == 0) {
            printf("Slhbond-Monitor %s\n", SLH_VERSION_STRING);
            return 1;
        }

        if (strcmp(name, "-c") == 0 || strcmp(name, "--config") == 0) {
            value = option_value(argc, argv, &i, inline_value);
            if (!value) {
                fprintf(stderr, "%s: %s requires a value\n", prog, name);
                return -1;
            }
            if (config_load_file(cfg, value) != 0) {
                fprintf(stderr, "%s: cannot read config file '%s'\n", prog, value);
                return -1;
            }
            continue;
        }
        if (strcmp(name, "-f") == 0 || strcmp(name, "--foreground") == 0) {
            cfg->foreground = 1;
            continue;
        }
        if (strcmp(name, "--no-update-check") == 0) {
            cfg->update_enabled = 0;
            continue;
        }

        value = option_value(argc, argv, &i, inline_value);
        if (!value) {
            fprintf(stderr, "%s: %s requires a value\n", prog, name);
            return -1;
        }

        if (strcmp(name, "-b") == 0 || strcmp(name, "--bind") == 0) {
            slh_strlcpy(cfg->bind_address, value, sizeof(cfg->bind_address));
        } else if (strcmp(name, "-p") == 0 || strcmp(name, "--port") == 0) {
            cfg->port = atoi(value);
        } else if (strcmp(name, "-r") == 0 || strcmp(name, "--docroot") == 0) {
            slh_strlcpy(cfg->doc_root, value, sizeof(cfg->doc_root));
        } else if (strcmp(name, "--state-dir") == 0) {
            slh_strlcpy(cfg->state_dir, value, sizeof(cfg->state_dir));
        } else if (strcmp(name, "-t") == 0 || strcmp(name, "--threads") == 0) {
            cfg->worker_threads = atoi(value);
        } else if (strcmp(name, "-l") == 0 || strcmp(name, "--log-file") == 0) {
            slh_strlcpy(cfg->log_file, value, sizeof(cfg->log_file));
        } else if (strcmp(name, "-L") == 0 || strcmp(name, "--log-level") == 0) {
            log_level_t level;
            if (log_level_parse(value, &level) != 0) {
                fprintf(stderr, "%s: unknown log level '%s'\n", prog, value);
                return -1;
            }
            cfg->log_level = level;
        } else if (strcmp(name, "--update-manifest") == 0) {
            slh_strlcpy(cfg->update_manifest, value, sizeof(cfg->update_manifest));
            cfg->update_enabled = value[0] != '\0';
        } else if (strcmp(name, "--update-interval") == 0) {
            cfg->update_interval_sec = atoi(value);
        } else {
            fprintf(stderr, "%s: unknown option '%s' (try --help)\n", prog, name);
            return -1;
        }
    }

    config_normalise(cfg);
    return 0;
}
