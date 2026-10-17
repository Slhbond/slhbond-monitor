/*
 * events.c - boot / shutdown / SSH login history.
 *
 * See events.h. The utmp database is a fixed-size binary record file; we use
 * the platform's own <utmp.h> layout rather than hardcoding offsets, so the
 * same code reads it on 32- and 64-bit systems. A byte offset is persisted so
 * a scan only reads records appended since the last one.
 */
#include "events.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#if defined(__has_include)
#  if __has_include(<utmp.h>)
#    include <utmp.h>
#    define SLH_HAVE_UTMP 1
#  endif
#endif

#ifndef SLH_HAVE_UTMP
#  define SLH_HAVE_UTMP 0
#endif

#define EVENTS_FILE_MAX 300
#define WTMP_PATHS      "/var/log/wtmp"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static event_record_t  g_rec[SLH_MAX_EVENTS];
static size_t          g_count;      /* valid entries, newest last */
static size_t          g_head;       /* ring write index           */
static char            g_log_path[640];
static char            g_off_path[640];
static char            g_bootid_path[640];
static int64_t         g_boot_ts;
static int64_t         g_last_event_ts;
static off_t           g_wtmp_off;
static int             g_login_ok;
static int             g_new_boot;
static char            g_login_why[160];
static uint64_t        g_disk_bytes;

/* Bumped whenever the meaning of a stored row changes.
 * v1 labelled service restarts as 关机, which was simply wrong. */
#define EVENTS_FORMAT "v2"

/* ------------------------------------------------------------------ */
/* Naming                                                              */
/* ------------------------------------------------------------------ */

const char *events_type_name(int type)
{
    switch (type) {
    case EVENT_BOOT:          return "开机";
    case EVENT_SHUTDOWN:      return "关机";
    case EVENT_LOGIN:         return "登录";
    case EVENT_LOGOUT:        return "登出";
    case EVENT_SERVICE_START: return "服务启动";
    case EVENT_SERVICE_STOP:  return "服务停止";
    default: return "未知";
    }
}

static const char *type_key(int type)
{
    switch (type) {
    case EVENT_BOOT:          return "boot";
    case EVENT_SHUTDOWN:      return "shutdown";
    case EVENT_LOGIN:         return "login";
    case EVENT_LOGOUT:        return "logout";
    case EVENT_SERVICE_START: return "service_start";
    case EVENT_SERVICE_STOP:  return "service_stop";
    default: return "?";
    }
}

static int type_from_key(const char *key)
{
    if (strcmp(key, "boot") == 0) return EVENT_BOOT;
    if (strcmp(key, "shutdown") == 0) return EVENT_SHUTDOWN;
    if (strcmp(key, "login") == 0) return EVENT_LOGIN;
    if (strcmp(key, "logout") == 0) return EVENT_LOGOUT;
    if (strcmp(key, "service_start") == 0) return EVENT_SERVICE_START;
    if (strcmp(key, "service_stop") == 0) return EVENT_SERVICE_STOP;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Ordering                                                            */
/* ------------------------------------------------------------------ */

/*
 * The ring holds records in insertion order, but a boot event is back-dated
 * to the real kernel start time, which can be days earlier than records that
 * were already there. Both the log file and the API must therefore be sorted
 * by timestamp, or the boot would appear as the newest entry.
 */
static int cmp_asc(const void *a, const void *b)
{
    const event_record_t *x = a;
    const event_record_t *y = b;

    if (x->ts < y->ts) return -1;
    if (x->ts > y->ts) return 1;
    return 0;
}

/** Copy the ring out oldest-first, sorted by timestamp. */
static size_t ordered_locked(event_record_t *out, size_t max)
{
    size_t n = g_count < max ? g_count : max;
    size_t i;

    for (i = 0; i < n; i++)
        out[i] = g_rec[(g_head + SLH_MAX_EVENTS - g_count + i) % SLH_MAX_EVENTS];
    if (n > 1)
        qsort(out, n, sizeof(*out), cmp_asc);
    return n;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static void save_locked(void)
{
    event_record_t sorted[SLH_MAX_EVENTS];
    size_t         n;
    FILE          *fp;
    size_t         i;

    if (g_log_path[0] == '\0')
        return;
    fp = fopen(g_log_path, "w");
    if (!fp) {
        LOG_WARN("cannot write %s: %s", g_log_path, strerror(errno));
        return;
    }
    fprintf(fp, "# " EVENTS_FORMAT " epoch\ttype\tunclean\tuser\tip\tdetail\n");

    n = ordered_locked(sorted, SLH_MAX_EVENTS);
    for (i = 0; i < n; i++) {
        const event_record_t *r = &sorted[i];

        /* Tabs inside a field would corrupt the row, so they become spaces. */
        fprintf(fp, "%lld\t%s\t%d\t%s\t%s\t%s\n",
                (long long)r->ts, type_key(r->type), r->unclean,
                r->user, r->ip, r->detail);
    }
    if (fclose(fp) == 0) {
        struct stat st;

        if (stat(g_log_path, &st) == 0)
            g_disk_bytes = (uint64_t)st.st_size;
    }
}

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
    }
}

static void add_locked(int64_t ts, int type, const char *user, const char *ip,
                       const char *detail, int unclean)
{
    event_record_t *r = &g_rec[g_head];

    memset(r, 0, sizeof(*r));
    r->ts = ts;
    r->type = type;
    r->unclean = unclean;
    slh_strlcpy(r->user, user ? user : "", sizeof(r->user));
    slh_strlcpy(r->ip, ip ? ip : "", sizeof(r->ip));
    slh_strlcpy(r->detail, detail ? detail : "", sizeof(r->detail));
    sanitize(r->user);
    sanitize(r->ip);
    sanitize(r->detail);

    g_head = (g_head + 1) % SLH_MAX_EVENTS;
    if (g_count < SLH_MAX_EVENTS)
        g_count++;
    if (ts > g_last_event_ts)
        g_last_event_ts = ts;
}

static void load_locked(void)
{
    FILE *fp;
    char  line[384];
    int   first = 1;
    int   legacy = 0;      /* file predates the machine-boot fix */

    fp = fopen(g_log_path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char           *fields[6];
        int             n = 0;
        char           *p = line;
        event_record_t  r;
        int             type;

        if (line[0] == '#') {
            if (first)
                legacy = strstr(line, EVENTS_FORMAT) == NULL;
            first = 0;
            continue;
        }
        first = 0;
        memset(&r, 0, sizeof(r));
        while (n < 6) {
            char *tab = strchr(p, '\t');

            fields[n++] = p;
            if (!tab)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n < 6)
            continue;
        fields[5][strcspn(fields[5], "\r\n")] = '\0';
        type = type_from_key(fields[1]);
        if (type < 0)
            continue;
        /*
         * Older rows called every service restart "开机"/"关机". Drop just
         * those two types - the login history in the same file is still good
         * and there is no reason to throw it away.
         */
        if (legacy && (type == EVENT_BOOT || type == EVENT_SHUTDOWN))
            continue;
        r.ts = (int64_t)strtoll(fields[0], NULL, 10);
        r.type = type;
        r.unclean = (int)strtol(fields[2], NULL, 10);
        slh_strlcpy(r.user, fields[3], sizeof(r.user));
        slh_strlcpy(r.ip, fields[4], sizeof(r.ip));
        slh_strlcpy(r.detail, fields[5], sizeof(r.detail));

        g_rec[g_head] = r;
        g_head = (g_head + 1) % SLH_MAX_EVENTS;
        if (g_count < SLH_MAX_EVENTS)
            g_count++;
        if (r.ts > g_last_event_ts)
            g_last_event_ts = r.ts;
    }
    fclose(fp);
    if (g_count)
        LOG_INFO("events: restored %zu record(s)", g_count);
}

static void load_offset_locked(void)
{
    FILE *fp;
    long  v = 0;

    fp = fopen(g_off_path, "r");
    if (!fp)
        return;
    if (fscanf(fp, "%ld", &v) == 1 && v >= 0)
        g_wtmp_off = (off_t)v;
    fclose(fp);
}

static void save_offset_locked(void)
{
    FILE *fp = fopen(g_off_path, "w");

    if (!fp)
        return;
    fprintf(fp, "%ld\n", (long)g_wtmp_off);
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* utmp scanning                                                       */
/* ------------------------------------------------------------------ */

#if SLH_HAVE_UTMP

/** Best-effort printable host string; ut_host may hold "ip:display". */
static void utmp_host(const struct utmp *u, char *out, size_t out_size)
{
    char raw[sizeof(u->ut_host) + 1];
    size_t i;

    memcpy(raw, u->ut_host, sizeof(u->ut_host));
    raw[sizeof(u->ut_host)] = '\0';
    /* ut_host is not guaranteed NUL-terminated when it fills the field. */
    for (i = 0; i < sizeof(u->ut_host) && raw[i]; i++)
        continue;
    raw[i] = '\0';
    if (raw[0] == '\0') {
        slh_strlcpy(out, "", out_size);
        return;
    }
    /* "192.168.5.18:0.0" -> "192.168.5.18" */
    {
        char *colon = strchr(raw, ':');

        if (colon)
            *colon = '\0';
    }
    slh_strlcpy(out, raw, out_size);
}

static void utmp_user(const struct utmp *u, char *out, size_t out_size)
{
    char raw[sizeof(u->ut_user) + 1];
    size_t i;

    memcpy(raw, u->ut_user, sizeof(u->ut_user));
    raw[sizeof(u->ut_user)] = '\0';
    for (i = 0; i < sizeof(u->ut_user) && raw[i]; i++)
        continue;
    raw[i] = '\0';
    slh_strlcpy(out, raw, out_size);
}

static void utmp_line(const struct utmp *u, char *out, size_t out_size)
{
    char raw[sizeof(u->ut_line) + 1];
    size_t i;

    memcpy(raw, u->ut_line, sizeof(u->ut_line));
    raw[sizeof(u->ut_line)] = '\0';
    for (i = 0; i < sizeof(u->ut_line) && raw[i]; i++)
        continue;
    raw[i] = '\0';
    slh_strlcpy(out, raw, out_size);
}

/**
 * Read records appended since the last scan. The offset is only advanced past
 * what was actually parsed, so a partial record at the end of the file is
 * revisited on the next pass instead of being skipped.
 *
 * Returns how many rows were added, so the caller can skip the disk write when
 * a poll found nothing - this runs every 10 seconds.
 */
static int scan_wtmp_locked(void)
{
    FILE       *fp;
    struct stat st;
    struct utmp u;
    size_t      before = g_count;

    fp = fopen(WTMP_PATHS, "rb");
    if (!fp) {
        g_login_ok = 0;
        snprintf(g_login_why, sizeof(g_login_why),
                 "无法读取 %s：%s", WTMP_PATHS, strerror(errno));
        return 0;
    }
    if (fstat(fileno(fp), &st) != 0) {
        fclose(fp);
        g_login_ok = 0;
        snprintf(g_login_why, sizeof(g_login_why), "无法读取 %s 的状态", WTMP_PATHS);
        return 0;
    }
    /* Rotation or truncation: start over rather than seek past the end. */
    if ((off_t)st.st_size < g_wtmp_off)
        g_wtmp_off = 0;

    if (fseeko(fp, g_wtmp_off, SEEK_SET) != 0) {
        fclose(fp);
        g_login_ok = 0;
        snprintf(g_login_why, sizeof(g_login_why), "无法定位 %s", WTMP_PATHS);
        return 0;
    }

    while (fread(&u, sizeof(u), 1, fp) == 1) {
        char    user[64];
        char    ip[80];
        char    line[40];
        char    detail[96];
        int64_t ts = (int64_t)u.ut_tv.tv_sec;

        g_wtmp_off += (off_t)sizeof(u);
        if (ts <= 0)
            continue;

        switch (u.ut_type) {
        case USER_PROCESS:
            utmp_user(&u, user, sizeof(user));
            utmp_host(&u, ip, sizeof(ip));
            utmp_line(&u, line, sizeof(line));
            if (user[0] == '\0')
                break;
            snprintf(detail, sizeof(detail), "终端 %s", line[0] ? line : "-");
            add_locked(ts, EVENT_LOGIN, user, ip, detail, 0);
            break;
        case DEAD_PROCESS:
            utmp_user(&u, user, sizeof(user));
            utmp_host(&u, ip, sizeof(ip));
            utmp_line(&u, line, sizeof(line));
            if (user[0] == '\0')
                break;
            snprintf(detail, sizeof(detail), "终端 %s", line[0] ? line : "-");
            add_locked(ts, EVENT_LOGOUT, user, ip, detail, 0);
            break;
        default:
            break;
        }
    }
    g_wtmp_off = (off_t)st.st_size;
    fclose(fp);
    g_login_ok = 1;
    g_login_why[0] = '\0';
    if (g_count != before)
        save_offset_locked();
    return (int)(g_count - before);
}

#else  /* !SLH_HAVE_UTMP */

static int scan_wtmp_locked(void)
{
    g_login_ok = 0;
    snprintf(g_login_why, sizeof(g_login_why),
             "该系统的 <utmp.h> 不可用，无法解析登录记录");
    return 0;
}

#endif /* SLH_HAVE_UTMP */

/* ------------------------------------------------------------------ */
/* Boot / shutdown                                                     */
/* ------------------------------------------------------------------ */

static int64_t read_boot_time(void)
{
    char    buf[64];
    double  up = 0.0;
    ssize_t n;

    n = slh_read_file("/proc/uptime", buf, sizeof(buf));
    if (n <= 0)
        return 0;
    if (sscanf(buf, "%lf", &up) != 1 || up < 0.0)
        return 0;
    return slh_now_sec() - (int64_t)up;
}

/**
 * The kernel regenerates this on every boot, so comparing it against the value
 * we stored last time is what distinguishes "the machine rebooted" from "our
 * own service was restarted".
 */
static void read_boot_id(char *out, size_t out_size)
{
    out[0] = '\0';
    if (!slh_read_text("/proc/sys/kernel/random/boot_id", out, out_size))
        out[0] = '\0';
}

static void load_prev_boot_id_locked(char *out, size_t out_size)
{
    out[0] = '\0';
    (void)slh_read_text(g_bootid_path, out, out_size);
}

static void save_boot_id_locked(const char *id)
{
    FILE *fp = fopen(g_bootid_path, "w");

    if (!fp)
        return;
    fprintf(fp, "%s\n", id);
    fclose(fp);
}

/**
 * Newest *service* stop, i.e. the last moment this daemon was asked to exit.
 *
 * On a normal power-off systemd sends us SIGTERM, so that timestamp is the
 * machine's shutdown time. We cannot tell a shutdown from a restart at that
 * moment, which is why it is only promoted once the next boot proves the
 * machine really went down.
 */
static int last_service_stop_locked(int64_t after, int64_t *out_ts)
{
    size_t i;

    for (i = 0; i < g_count; i++) {
        size_t idx = (g_head + SLH_MAX_EVENTS - 1 - i) % SLH_MAX_EVENTS;

        if (g_rec[idx].type == EVENT_SERVICE_STOP && g_rec[idx].ts >= after) {
            if (out_ts)
                *out_ts = g_rec[idx].ts;
            return 1;
        }
    }
    return 0;
}

/** Timestamp of the newest machine boot we already have, 0 when none. */
static int64_t last_boot_locked(void)
{
    size_t i;

    for (i = 0; i < g_count; i++) {
        size_t idx = (g_head + SLH_MAX_EVENTS - 1 - i) % SLH_MAX_EVENTS;

        if (g_rec[idx].type == EVENT_BOOT)
            return g_rec[idx].ts;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int events_start(const config_t *cfg)
{
    char    boot_id[64];
    char    prev_id[64];
    int64_t prev_boot;

    snprintf(g_log_path, sizeof(g_log_path), "%s/events.log", cfg->state_dir);
    snprintf(g_off_path, sizeof(g_off_path), "%s/wtmp.offset", cfg->state_dir);
    snprintf(g_bootid_path, sizeof(g_bootid_path), "%s/boot-id", cfg->state_dir);

    g_boot_ts = read_boot_time();
    read_boot_id(boot_id, sizeof(boot_id));

    pthread_mutex_lock(&g_lock);
    load_locked();
    load_offset_locked();

    /*
     * Rewind the login cursor when the log holds no login rows at all: that
     * happens on a first run, and after an upgrade that dropped rows. Without
     * this the existing /var/log/wtmp would be skipped for good and the panel
     * would claim nobody had ever logged in.
     */
    {
        size_t i;
        int    have_login = 0;

        for (i = 0; i < g_count; i++) {
            if (g_rec[i].type == EVENT_LOGIN || g_rec[i].type == EVENT_LOGOUT) {
                have_login = 1;
                break;
            }
        }
        if (!have_login)
            g_wtmp_off = 0;
    }

    prev_boot = last_boot_locked();
    load_prev_boot_id_locked(prev_id, sizeof(prev_id));

    /*
     * A different boot_id means the kernel started again - the only reliable
     * evidence that the *machine* went down, as opposed to our own restart.
     * A missing stored id means we have never looked before (first run, or an
     * upgrade from a build that did not track it), which is also a machine
     * boot as far as this log is concerned.
     */
    g_new_boot = boot_id[0] != '\0' &&
                 (prev_id[0] == '\0' || strcmp(boot_id, prev_id) != 0);

    if (g_new_boot && g_boot_ts > 0) {
        /* Only attribute a shutdown when there was an earlier boot to end. */
        if (prev_boot > 0) {
            int64_t stop_ts = 0;

            if (last_service_stop_locked(prev_boot, &stop_ts)) {
                add_locked(stop_ts, EVENT_SHUTDOWN, "", "",
                           "系统正常关闭（服务收到退出信号）", 0);
            } else {
                /*
                 * Nothing was written on the way down: power loss or a hard
                 * reset. We honestly do not know when it happened, so the time
                 * shown is the last boot we do know about rather than a
                 * made-up instant.
                 */
                add_locked(prev_boot, EVENT_SHUTDOWN, "", "",
                           "上一次运行没有留下退出记录，断电或强制重启；关机时刻无法确定",
                           1);
            }
        }

        {
            char detail[96];

            snprintf(detail, sizeof(detail), "内核启动于 %lld",
                     (long long)g_boot_ts);
            add_locked(g_boot_ts, EVENT_BOOT, "", "", detail, 0);
        }
        LOG_INFO("events: machine boot detected at %lld", (long long)g_boot_ts);
    } else if (g_boot_ts > 0) {
        LOG_INFO("events: same boot as before (uptime %lld s) - this start is a "
                 "service restart, not a power cycle",
                 (long long)(slh_now_sec() - g_boot_ts));
    }

    /*
     * Always persist the current id: storing it only when a new boot was
     * *detected* would mean never having a baseline, and every start would
     * look like a fresh machine boot.
     */
    if (boot_id[0])
        save_boot_id_locked(boot_id);

    /*
     * Always note our own start/stop so restarts stay visible as such.
     * "随系统启动" is decided by uptime, not by whether a new boot was just
     * detected: the very first run after installing this build also detects a
     * boot, but the service itself was started by hand minutes earlier.
     */
    {
        int64_t uptime = g_boot_ts > 0 ? slh_now_sec() - g_boot_ts : -1;

        add_locked(slh_now_sec(), EVENT_SERVICE_START, "", "",
                   (uptime >= 0 && uptime < 300) ? "随系统启动"
                                                 : "监控服务被重启", 0);
    }

    scan_wtmp_locked();
    save_locked();
    pthread_mutex_unlock(&g_lock);

    if (g_login_ok)
        LOG_INFO("events: login database tracked, %zu record(s) total", g_count);
    else
        LOG_WARN("events: login records unavailable - %s", g_login_why);
    return 0;
}

void events_poll(void)
{
    int added;

    pthread_mutex_lock(&g_lock);
    added = scan_wtmp_locked();
    /* Nothing new: do not rewrite the log every 10 seconds for no reason. */
    if (added > 0)
        save_locked();
    pthread_mutex_unlock(&g_lock);
}

void events_stop(void)
{
    pthread_mutex_lock(&g_lock);
    /*
     * Deliberately NOT stored as 关机: the very same signal arrives for a
     * plain `systemctl restart`, and calling that a shutdown is what made the
     * log useless. It becomes a shutdown only after the next boot proves the
     * machine really restarted.
     */
    add_locked(slh_now_sec(), EVENT_SERVICE_STOP, "", "", "监控服务退出", 0);
    save_locked();
    pthread_mutex_unlock(&g_lock);
}

int events_is_new_boot(void)
{
    int v;

    pthread_mutex_lock(&g_lock);
    v = g_new_boot;
    pthread_mutex_unlock(&g_lock);
    return v;
}

size_t events_list(event_record_t *out, size_t max, int include_service)
{
    event_record_t sorted[SLH_MAX_EVENTS];
    size_t         n;
    size_t         i;
    size_t         taken = 0;

    pthread_mutex_lock(&g_lock);
    n = ordered_locked(sorted, SLH_MAX_EVENTS);
    /* Newest first for the UI. */
    for (i = n; i > 0 && taken < max; i--) {
        const event_record_t *r = &sorted[i - 1];

        if (!include_service && !EVENT_IS_POWER(r->type) &&
            r->type != EVENT_LOGIN && r->type != EVENT_LOGOUT)
            continue;
        out[taken++] = *r;
    }
    pthread_mutex_unlock(&g_lock);
    return taken;
}

int events_login_status(char *why, size_t why_size)
{
    int ok;

    pthread_mutex_lock(&g_lock);
    ok = g_login_ok;
    if (!ok && why && why_size)
        slh_strlcpy(why, g_login_why, why_size);
    pthread_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}

int64_t events_boot_time(void)
{
    int64_t ts;

    pthread_mutex_lock(&g_lock);
    ts = g_boot_ts;
    pthread_mutex_unlock(&g_lock);
    return ts;
}

uint64_t events_disk_bytes(void)
{
    uint64_t n;

    pthread_mutex_lock(&g_lock);
    n = g_disk_bytes;
    pthread_mutex_unlock(&g_lock);
    return n;
}
