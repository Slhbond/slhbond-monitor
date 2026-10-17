/*
 * version.c - build version, version ordering and the update poller.
 */
#include "version.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "httpclient.h"
#include "json.h"
#include "log.h"
#include "mirror.h"

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
static int              g_daily_hour = 0;
/* 加速节点：前缀列表 + 是否允许清单走代理 */
static char             g_mirrors[512];
static int              g_via_mirror = 1;
/* 上一次拉清单用的是哪条路：-1 直连，>=1 是 cands 下标 */
static int              g_used_mirror_idx = -1;
static int              g_used_mirror_ms;

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

/* 清单只有几百字节，但国内直连 github.com 时「连接」阶段就可能要 20 秒以上，
 * 所以给足时间；它是后台任务，慢一点完全可以接受。 */
#define MANIFEST_TIMEOUT_MS      25000
/* 探测加速节点时单个节点的上限，以及「够快就不再测」的阈值 */
#define MIRROR_PROBE_MS          6000
#define MIRROR_GOOD_ENOUGH_MS    1500

/*
 * 拉清单。先直连官方，失败再看配置允不允许走加速节点。
 * 走代理时把用到的节点记下来，接口和日志都能说明白。
 */
static int read_manifest(const char *source, char *buf, size_t buf_size, char *err,
                         size_t err_size)
{
    if (!slh_starts_with(source, "http://") &&
        !slh_starts_with(source, "https://")) {
        if (slh_read_file(source, buf, buf_size) < 0) {
            snprintf(err, err_size, "无法读取 %.128s", source);
            return -1;
        }
        g_used_mirror_idx = -1;
        g_used_mirror_ms = 0;
        return 0;
    }

    /* 1) 直连官方 —— 清单是权威信息，能直连就绝不走第三方 */
    if (http_fetch(source, buf, buf_size, MANIFEST_TIMEOUT_MS, err, err_size) == 0) {
        g_used_mirror_idx = -1;
        g_used_mirror_ms = 0;
        return 0;
    }

    {
        char direct_err[192];

        slh_strlcpy(direct_err, err, sizeof(direct_err));

        if (!g_via_mirror || g_mirrors[0] == '\0') {
            snprintf(err, err_size, "直连失败（%.140s），且未启用加速节点",
                     direct_err);
            return -1;
        }

        /* 2) 退到加速节点：逐个探延迟，取第一个能拉到完整清单的 */
        {
            mirror_t cands[SLH_MAX_MIRRORS];
            int      n = mirror_parse(g_mirrors, cands, SLH_MAX_MIRRORS);
            int      i;

            for (i = 1; i < n; i++) {   /* 0 是直连，已经试过了 */
                char url[640];
                int  ms;

                snprintf(url, sizeof(url), "%s%s", cands[i].prefix, source);
                ms = mirror_probe(url, MIRROR_PROBE_MS);
                if (ms < 0)
                    continue;

                err[0] = '\0';
                if (http_fetch(url, buf, buf_size, MANIFEST_TIMEOUT_MS,
                               err, err_size) == 0) {
                    g_used_mirror_idx = i;
                    g_used_mirror_ms = ms;
                    LOG_WARN("update: 直连 GitHub 失败，清单取自加速节点 %s（%d ms）；"
                             "包体仍按官方清单里的 SHA-256 校验",
                             cands[i].prefix, ms);
                    return 0;
                }
            }
        }
    }

    /* 走到这里说明直连和所有节点都没成：先留一份直连的错误再覆盖 err */
    {
        char why[160];

        slh_strlcpy(why, err, sizeof(why));
        snprintf(err, err_size, "直连与所有加速节点都失败（直连：%.120s）", why);
    }
    return -1;
}

/**
 * Read an integer field from the flat manifest.
 *
 * The shared JSON helper only handles quoted strings, but "size" is written as
 * a bare number, and asking publishers to quote it would be a needless trap.
 */
static int64_t manifest_get_int(const char *json, const char *key)
{
    char        pattern[64];
    const char *p;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p)
        return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"')
        p++;
    if (!isdigit((unsigned char)*p) && *p != '-')
        return 0;
    return (int64_t)strtoll(p, NULL, 10);
}

static int secs_until_daily(int hour);

/**
 * Stamp the attempt and work out when the poller will look again, so the panel
 * can show "上次检查 / 下次检查" instead of leaving the user guessing whether
 * the nightly check is actually scheduled.
 */
static void note_check_locked(void)
{
    int64_t now = slh_now_sec();
    int64_t next = (g_interval_sec > 0) ? now + g_interval_sec : 0;
    int     d;

    slh_rfc3339_local((time_t)now, g_state.checked_at,
                      sizeof(g_state.checked_at));
    g_state.checked_ts = now;
    g_state.daily_hour = g_daily_hour;

    d = secs_until_daily(g_daily_hour);
    if (d > 0 && (next == 0 || now + d < next))
        next = now + d;

    /* 两种自动检查都关了：没有「下次」，给 0 让界面显示「仅手动」。 */
    g_state.next_check_at = next;
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
        note_check_locked();
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
        note_check_locked();
        pthread_mutex_unlock(&g_lock);
        LOG_WARN("update manifest %s has no usable 'version' field", manifest);
        return -1;
    }
    json_flat_get_str(body, "url", url, sizeof(url));
    json_flat_get_str(body, "notes", notes, sizeof(notes));
    {
        char    sha[80];
        int64_t size;

        sha[0] = '\0';
        json_flat_get_str(body, "sha256", sha, sizeof(sha));
        size = manifest_get_int(body, "size");

        pthread_mutex_lock(&g_lock);
        slh_strlcpy(g_state.release_sha256, sha, sizeof(g_state.release_sha256));
        g_state.release_size = size;
        pthread_mutex_unlock(&g_lock);
    }

    pthread_mutex_lock(&g_lock);
    slh_strlcpy(g_state.latest, latest, sizeof(g_state.latest));
    slh_strlcpy(g_state.release_url, url, sizeof(g_state.release_url));
    slh_strlcpy(g_state.notes, notes, sizeof(g_state.notes));
    note_check_locked();
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

/**
 * Seconds from now until the next `hour`:00 local time.
 *
 * The daily check and the interval check are separate promises - "also check
 * every night at midnight" is not the same as "check every 30 minutes" - so the
 * poller sleeps for whichever comes first.
 */
static int secs_until_daily(int hour)
{
    time_t    now = time(NULL);
    struct tm tm;

    if (hour < 0 || hour > 23)
        return -1;
    localtime_r(&now, &tm);

    {
        int now_sec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        int target = hour * 3600;
        int delta = target - now_sec;

        if (delta <= 0)
            delta += 86400;
        return delta;
    }
}

/**
 * Sleep until `deadline`, tolerating wakeups that are not ours.
 *
 * pthread_cond_timedwait may return before the deadline (a spurious wakeup, or
 * a signal delivered to the thread). Treating that as "time is up" turned into
 * an early check: an 11:00 daily check once fired at 10:38. The fix is the
 * textbook one - only stop waiting when the clock really reached the deadline.
 *
 * Must be called with g_lock held; returns with it held.
 */
static int wait_until_locked(struct timespec *deadline)
{
    for (;;) {
        struct timespec now;
        int             rc;

        if (g_stop)
            return -1;

        rc = pthread_cond_timedwait(&g_wake, &g_lock, deadline);
        clock_gettime(CLOCK_REALTIME, &now);

        if (now.tv_sec > deadline->tv_sec)
            return 0;
        if (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)
            return 0;
        (void)rc;      /* ETIMEDOUT before the deadline cannot happen, but a
                        * clock step backwards can; just wait again. */
    }
}

/**
 * Minimum spacing between automatic checks, in seconds.
 *
 * wait_until_locked already refuses to return before the deadline, so this is
 * belt-and-braces. It exists because a startup burst of ~33 checks in 8 s was
 * observed once and never reproduced: whatever made the deadline look past,
 * this makes it impossible for the poller to hammer the update server.
 */
#define AUTO_CHECK_MIN_GAP 5

static void *checker_main(void *arg)
{
    SLH_UNUSED(arg);

    int64_t last_check = 0;

    for (;;) {
        struct timespec deadline;
        int wait_sec;
        int daily_hour;
        int64_t now = slh_now_sec();

        if (last_check == 0 || now - last_check >= AUTO_CHECK_MIN_GAP) {
            version_check_now();
            last_check = slh_now_sec();
        } else {
            LOG_DEBUG("update: skipping check, only %lld s since the last one",
                      (long long)(now - last_check));
        }

        pthread_mutex_lock(&g_lock);
        wait_sec = g_interval_sec;      /* 0 = 关掉了按间隔检查 */
        daily_hour = g_daily_hour;
        pthread_mutex_unlock(&g_lock);

        /* 两种自动检查取更早的那个；0 表示那一种没开 */
        if (daily_hour >= 0) {
            int until_daily = secs_until_daily(daily_hour);

            if (until_daily > 0 && (wait_sec <= 0 || until_daily < wait_sec))
                wait_sec = until_daily;
        }
        if (wait_sec <= 0)
            wait_sec = 3600;            /* 都关了；线程本不该走到这里 */
        if (wait_sec < AUTO_CHECK_MIN_GAP)
            wait_sec = AUTO_CHECK_MIN_GAP;

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += wait_sec;

        LOG_DEBUG("update: sleeping %d s until %lld.%09ld",
                  wait_sec, (long long)deadline.tv_sec, deadline.tv_nsec);

        pthread_mutex_lock(&g_lock);
        {
            int rc = wait_until_locked(&deadline);
            int stop = g_stop;
            struct timespec woke;

            clock_gettime(CLOCK_REALTIME, &woke);
            pthread_mutex_unlock(&g_lock);
            LOG_DEBUG("update: woke rc=%d at %lld.%09ld (%+lld ms vs deadline)",
                      rc, (long long)woke.tv_sec, woke.tv_nsec,
                      (long long)(woke.tv_sec - deadline.tv_sec) * 1000 +
                      (woke.tv_nsec - deadline.tv_nsec) / 1000000);
            if (rc != 0 || stop)
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
    g_daily_hour = cfg->update_daily_hour;
    slh_strlcpy(g_mirrors, cfg->update_mirrors, sizeof(g_mirrors));
    g_via_mirror = cfg->update_manifest_via_mirror;
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

    /*
     * 两种自动检查都关掉时不起线程：没有定时任务可跑，
     * 面板上的「检查版本」按钮仍然可以随时手动触发。
     */
    if (cfg->update_interval_sec <= 0 && cfg->update_daily_hour < 0) {
        LOG_INFO("update: 定时检查已全部关闭，只能用面板上的「检查版本」按钮");
        return;
    }

    if (pthread_create(&g_thread, NULL, checker_main, NULL) != 0) {
        LOG_WARN("could not start the update checker thread");
        return;
    }
    g_thread_running = 1;
    LOG_INFO("update checker started (manifest=%s, interval=%s, daily=%s)",
             cfg->update_manifest,
             cfg->update_interval_sec > 0 ? "on" : "off",
             cfg->update_daily_hour >= 0 ? "on" : "off");
    if (cfg->update_daily_hour >= 0)
        LOG_INFO("update: next daily check in %d s (at %02d:00 local)",
                 secs_until_daily(cfg->update_daily_hour), cfg->update_daily_hour);
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
    json_add_str(sb, "release_sha256",
                 st->release_sha256[0] ? st->release_sha256 : NULL, &first);
    if (st->release_size > 0)
        json_add_int(sb, "release_size", st->release_size, &first);
    json_add_str(sb, "notes", st->notes[0] ? st->notes : NULL, &first);
    json_add_str(sb, "manifest", st->source[0] ? st->source : NULL, &first);
    json_add_str(sb, "checked_at",
                 st->checked_at[0] ? st->checked_at : NULL, &first);
    json_add_int(sb, "checked_ts", st->checked_ts, &first);
    json_add_int(sb, "next_check_at", st->next_check_at, &first);
    json_add_int(sb, "daily_hour", st->daily_hour, &first);
    json_add_int(sb, "interval_sec", g_interval_sec, &first);
    /*
     * 清单是从哪条路拉到的：直连还是某个加速节点。
     * 面板上要能看出「这次数据不是官方直连来的」。
     */
    if (g_used_mirror_idx >= 0) {
        mirror_t cands[SLH_MAX_MIRRORS];
        int      n = mirror_parse(g_mirrors, cands, SLH_MAX_MIRRORS);

        json_add_bool(sb, "manifest_via_mirror", 1, &first);
        if (g_used_mirror_idx < n)
            json_add_str(sb, "manifest_mirror",
                         cands[g_used_mirror_idx].prefix, &first);
        json_add_int(sb, "manifest_mirror_ms", g_used_mirror_ms, &first);
    } else {
        json_add_bool(sb, "manifest_via_mirror", 0, &first);
    }
    json_add_str(sb, "last_error",
                 st->last_error[0] ? st->last_error : NULL, &first);
    json_add_str(sb, "name", "Slhbond-Monitor", &first);
    json_add_int(sb, "poll_interval_ms", cfg ? cfg->poll_ms : SLH_POLL_MS_DEFAULT,
                 &first);
    sb_appendc(sb, '}');
}
