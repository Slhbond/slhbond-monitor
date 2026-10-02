/*
 * main.c - Slhbond-Monitor: process start-up, wiring and shutdown.
 *
 * Wire-up order matters:
 *   1. configuration (file + flags)
 *   2. logging
 *   3. block the shutdown signals BEFORE any thread exists, so every worker
 *      inherits the mask and one dedicated thread can use sigwait(3)
 *   4. detach if not in foreground mode
 *   5. start the update checker, the API cache and the HTTP server
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "api.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "router.h"
#include "util.h"
#include "version.h"

static http_server_t *g_server = NULL;

/* ------------------------------------------------------------------ */
/* Daemonising                                                         */
/* ------------------------------------------------------------------ */

static int daemonize(void)
{
    pid_t pid;
    int   fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);                 /* the launching shell gets its prompt back */

    if (setsid() < 0)
        return -1;

    pid = fork();                 /* second fork: never re-acquire a terminal */
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);

    umask(022);
    if (chdir("/") != 0)
        return -1;

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

static void *signal_thread_main(void *arg)
{
    sigset_t *set = arg;

    for (;;) {
        int sig = 0;

        if (sigwait(set, &sig) != 0)
            continue;
        if (sig == SIGTERM || sig == SIGINT || sig == SIGQUIT) {
            LOG_INFO("received signal %d - shutting down", sig);
            http_server_stop(g_server);
            return NULL;
        }
        LOG_DEBUG("ignoring signal %d", sig);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/** 1 when argv carries -c/--config, so the default file must not be loaded. */
static int argv_selects_config(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0 ||
            slh_starts_with(argv[i], "--config="))
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const route_entry_t routes[] = {
        { "GET",  "/api/v1/system",     api_handle_system     },
        { "GET",  "/api/v1/cpu",        api_handle_cpu        },
        { "GET",  "/api/v1/gpu",        api_handle_gpu        },
        { "GET",  "/api/v1/network",    api_handle_network    },
        { "GET",  "/api/v1/npu",        api_handle_npu        },
        { "GET",  "/api/v1/disks",      api_handle_disks      },
        /* The one write endpoint: ask for a disk health check to run now. */
        { "POST", "/api/v1/disk/check", api_handle_disk_check },
        { "GET",  "/api/v1/metrics",    api_handle_metrics    },
        { "GET",  "/api/v1/version",    api_handle_version    },
        { "GET",  "/api/v1/health",     api_handle_health     },
        { "GET",  "/api/v1/status",     api_handle_status     },
        { "GET",  "/api/v1",            api_handle_status     },
    };

    config_t       cfg;
    api_context_t  api;
    router_t       router;
    http_server_t *server = NULL;
    sigset_t       sigset;
    pthread_t      sig_thread;
    char           err[256];
    int            rc;

    config_defaults(&cfg);

    /* The packaged config file is used unless the caller names another one. */
    if (!argv_selects_config(argc, argv) && slh_file_exists(SLH_CONFIG_PATH_DEFAULT)) {
        if (config_load_file(&cfg, SLH_CONFIG_PATH_DEFAULT) != 0)
            fprintf(stderr, "warning: cannot read %s\n", SLH_CONFIG_PATH_DEFAULT);
    }

    rc = config_parse_args(&cfg, argc, argv);
    if (rc > 0)
        return 0;                       /* --help / --version */
    if (rc < 0)
        return 2;                       /* usage error already reported */
    config_normalise(&cfg);

    if (log_init(cfg.log_level, cfg.log_file) != 0)
        fprintf(stderr, "warning: cannot open log file '%s'\n", cfg.log_file);
    LOG_INFO("Slhbond-Monitor %s starting", SLH_VERSION_STRING);

    /* Block the shutdown signals before the first thread is created. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGQUIT);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL) != 0)
        LOG_WARN("could not block shutdown signals: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN);

    if (!cfg.foreground) {
        if (daemonize() != 0) {
            LOG_ERROR("could not detach into the background: %s", strerror(errno));
            log_close();
            return 1;
        }
        LOG_INFO("running as a background daemon");
    }

    version_state_init();
    version_checker_start(&cfg);

    /* The disk health worker owns the one subprocess we ever spawn. */
    disk_init(cfg.state_dir);

    api_context_init(&api, &cfg);
    /* The API context - never the router itself - is what handlers receive. */
    router_init(&router, routes, sizeof(routes) / sizeof(routes[0]), cfg.doc_root,
                &api);

    server = http_server_create(&cfg, router_dispatch, &router, err, sizeof(err));
    if (!server) {
        LOG_ERROR("cannot start HTTP server: %s", err);
        api_context_destroy(&api);
        version_checker_stop();
        log_close();
        return 1;
    }

    g_server = server;
    if (pthread_create(&sig_thread, NULL, signal_thread_main, &sigset) != 0) {
        LOG_WARN("could not start the signal thread; Ctrl-C will not be handled");
        sig_thread = 0;
    }

    LOG_INFO("web root: %s", cfg.doc_root);
    rc = http_server_run(server);
    LOG_INFO("HTTP server stopped");

    if (sig_thread) {
        pthread_cancel(sig_thread);
        pthread_join(sig_thread, NULL);
    }

    version_checker_stop();
    disk_shutdown();
    api_context_destroy(&api);
    http_server_destroy(server);
    LOG_INFO("Slhbond-Monitor stopped");
    log_close();
    return rc == 0 ? 0 : 1;
}
