/*
 * version.c - build version, version ordering and the update poller.
 */
#include "version.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "httpclient.h"
#include "json.h"
#include "log.h"

#define MANIFEST_MAX 8192

/* ------------------------------------------------------------------ */
/* Version ordering                                                    */
/* ------------------------------------------------------------------ */

/** Read one numeric component; advances *p past it. Returns -1 when absent. */
static long next_component(const char **p)
{
    long value = 0;
    int  digits = 0;

    while (**p >= '0' && **p <= '9') {
        value = value * 10 + (**p - '0');
        (*p)++;
        digits++;
    }
    return digits ? value : -1;
}

int version_compare(const char *a, const char *b)
{
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";

    if (*pa == 'v' || *pa == 'V')
        pa++;
    if (*pb == 'v' || *pb == 'V')
        pb++;

    for (;;) {
        long va = next_component(&pa);
        long vb = next_component(&pb);

        if (va < 0 && vb < 0)
            break;
        if (va < 0)
            va = 0;
        if (vb < 0)
            vb = 0;
        if (va != vb)
            return va < vb ? -1 : 1;

        if (*pa == '.')
            pa++;
        if (*pb == '.')
            pb++;
        if (*pa == '\0' && *pb == '\0')
            break;
    }

    /* Equal numbers: a pre-release suffix ("-rc1") sorts before a plain release. */
    {
        int a_suffix = (*pa == '-' || *pa == '+') ? 1 : 0;
        int b_suffix = (*pb == '-' || *pb == '+') ? 1 : 0;

        if (a_suffix != b_suffix)
            return a_suffix ? -1 : 1;
        if (a_suffix && b_suffix) {
            int rc = strcmp(pa, pb);
            return rc < 0 ? -1 : (rc > 0 ? 1 : 0);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static version_state_t  g_state;

static pthread_t        g_thread;
static int              g_thread_running;
static pthread_cond_t   g_wake = PTHREAD_COND_INITIALIZER;
static int              g_stop;
static char             g_manifest[512];
static int              g_interval_sec = SLH_UPDATE_SEC_DEFAULT;

void version_state_init(void)
{
    pthread_mutex_lock(&g_lock);
    memset(&g_state, 0, sizeof(g_state));
    slh_strlcpy(g_state.current, SLH_VERSION_STRING, sizeof(g_state.current));
    slh_strlcpy(g_state.latest, SLH_VERSION_STRING, sizeof(g_state.latest));
    g_state.update_available = 0;
    g_state.enabled = 0;
    pthread_mutex_unlock(&g_lock);
}

void version_state_get(version_state_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_state;
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Manifest polling                                                    */
/* ------------------------------------------------------------------ */

static int read_manifest(const char *source, char *buf, size_t buf_size, char *err,
                         size_t err_size)
{
    if (slh_starts_with(source, "http://") || slh_starts_with(source, "https://")) {
        http_client_response_t resp;
        int rc;

        memset(&resp, 0, sizeof(resp));
        rc = http_get(source, &resp, 8000);
        if (rc != 0) {
            snprintf(err, err_size, "请求失败 (%.96s)", resp.error);
            http_client_response_free(&resp);
            return -1;
        }
        if (resp.status != 200) {
            snprintf(err, err_size, "HTTP %d", resp.status);
            http_client_response_free(&resp);
            return -1;
        }
        slh_strlcpy(buf, resp.body ? resp.body : "", buf_size);
        http_client_response_free(&resp);
        return 0;
    }

    if (slh_read_file(source, buf, buf_size) < 0) {
        snprintf(err, err_size, "无法读取 %.128s", source);
        return -1;
    }
    return 0;
}

int version_check_now(void)
{
    char            manifest[512];
    char            body[MANIFEST_MAX];
    char            latest[64];
    char            url[512];
    char            notes[512];
    char            err[192];
    char            stamp[48];
    int             enabled;

    pthread_mutex_lock(&g_lock);
    enabled = g_state.enabled;
    slh_strlcpy(manifest, g_manifest, sizeof(manifest));
    pthread_mutex_unlock(&g_lock);

    if (!enabled || manifest[0] == '\0')
        return -1;

    slh_rfc3339_local((time_t)slh_now_sec(), stamp, sizeof(stamp));
    err[0] = '\0';

    if (read_manifest(manifest, body, sizeof(body), err, sizeof(err)) != 0) {
        pthread_mutex_lock(&g_lock);
        g_state.last_check_ok = 0;
        slh_strlcpy(g_state.last_error, err, sizeof(g_state.last_error));
        slh_strlcpy(g_state.checked_at, stamp, sizeof(g_state.checked_at));
        pthread_mutex_unlock(&g_lock);
        LOG_WARN("update check failed for %s: %s", manifest, err);
        return -1;
    }

    latest[0] = url[0] = notes[0] = '\0';
    if (!json_flat_get_str(body, "version", latest, sizeof(latest)) || latest[0] == '\0') {
        pthread_mutex_lock(&g_lock);
        g_state.last_check_ok = 0;
        slh_strlcpy(g_state.last_error, "清单缺少 version 字段",
                    sizeof(g_state.last_error));
        slh_strlcpy(g_state.checked_at, stamp, sizeof(g_state.checked_at));
        pthread_mutex_unlock(&g_lock);
        LOG_WARN("update manifest %s has no usable 'version' field", manifest);
        return -1;
    }
    json_flat_get_str(body, "url", url, sizeof(url));
    json_flat_get_str(body, "notes", notes, sizeof(notes));

    pthread_mutex_lock(&g_lock);
    slh_strlcpy(g_state.latest, latest, sizeof(g_state.latest));
    slh_strlcpy(g_state.release_url, url, sizeof(g_state.release_url));
    slh_strlcpy(g_state.notes, notes, sizeof(g_state.notes));
    slh_strlcpy(g_state.checked_at, stamp, sizeof(g_state.checked_at));
    slh_strlcpy(g_state.last_error, "", sizeof(g_state.last_error));
    g_state.last_check_ok = 1;
    g_state.update_available = version_compare(latest, g_state.current) > 0 ? 1 : 0;
    {
        int available = g_state.update_available;
        char current[64];
        slh_strlcpy(current, g_state.current, sizeof(current));
        pthread_mutex_unlock(&g_lock);
        LOG_INFO("update check: current %s, latest %s, update %s",
                 current, latest, available ? "available" : "not needed");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Background poller                                                   */
/* ------------------------------------------------------------------ */

static void *checker_main(void *arg)
{
    SLH_UNUSED(arg);

    for (;;) {
        struct timespec deadline;
        int wait_sec;

        version_check_now();

        pthread_mutex_lock(&g_lock);
        wait_sec = g_interval_sec;
        pthread_mutex_unlock(&g_lock);

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += wait_sec;

        pthread_mutex_lock(&g_lock);
        if (!g_stop)
            pthread_cond_timedwait(&g_wake, &g_lock, &deadline);
        {
            int stop = g_stop;
            pthread_mutex_unlock(&g_lock);
            if (stop)
                break;
        }
    }
    return NULL;
}

void version_checker_start(const config_t *cfg)
{
    pthread_mutex_lock(&g_lock);
    g_stop = 0;
    g_interval_sec = cfg->update_interval_sec;
    slh_strlcpy(g_manifest, cfg->update_manifest, sizeof(g_manifest));
    slh_strlcpy(g_state.source, cfg->update_manifest, sizeof(g_state.source));
    g_state.enabled = cfg->update_enabled && cfg->update_manifest[0] != '\0';
    {
        int enabled = g_state.enabled;
        pthread_mutex_unlock(&g_lock);

        if (!enabled) {
            LOG_INFO("update checking disabled");
            return;
        }
    }

    if (pthread_create(&g_thread, NULL, checker_main, NULL) != 0) {
        LOG_WARN("could not start the update checker thread");
        return;
    }
    g_thread_running = 1;
    LOG_INFO("update checker started (manifest=%s, interval=%ds)",
             cfg->update_manifest, cfg->update_interval_sec);
}

void version_checker_stop(void)
{
    if (!g_thread_running)
        return;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_thread_running = 0;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

void version_to_json(strbuf_t *sb, const version_state_t *st, const config_t *cfg)
{
    int first = 1;

    sb_appendc(sb, '{');
    json_add_str(sb, "current", st->current, &first);
    json_add_str(sb, "latest", st->latest, &first);
    json_add_bool(sb, "update_available", st->update_available, &first);
    json_add_bool(sb, "update_check_enabled", st->enabled, &first);
    json_add_bool(sb, "last_check_ok", st->last_check_ok, &first);
    json_add_str(sb, "release_url",
                 st->release_url[0] ? st->release_url : NULL, &first);
    json_add_str(sb, "notes", st->notes[0] ? st->notes : NULL, &first);
    json_add_str(sb, "manifest", st->source[0] ? st->source : NULL, &first);
    json_add_str(sb, "checked_at",
                 st->checked_at[0] ? st->checked_at : NULL, &first);
    json_add_str(sb, "last_error",
                 st->last_error[0] ? st->last_error : NULL, &first);
    json_add_str(sb, "name", "Slhbond-Monitor", &first);
    json_add_int(sb, "poll_interval_ms", cfg ? cfg->poll_ms : SLH_POLL_MS_DEFAULT,
                 &first);
    sb_appendc(sb, '}');
}
