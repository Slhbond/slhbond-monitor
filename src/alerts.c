/*
 * alerts.c - threshold breaches recorded to disk.
 *
 * See alerts.h. One state machine per metric:
 *
 *   normal  --value >= limit for hold_sec-->  breach (record opened)
 *   breach  --value <  limit - margin------>  normal (record closed)
 *
 * `margin` exists purely to stop a value sitting exactly on the line from
 * opening and closing records over and over.
 */
#include "alerts.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

typedef struct {
    int64_t above_since;   /* first ts of the current above-limit run, 0 none */
    int64_t last_ts;       /* newest sample seen (for gap detection)      */
    int     active;        /* a record has been opened                    */
    int64_t started_ts;
    double  peak;          /* worst value of the current run              */
    double  saved_peak;    /* worst value already written to disk         */
    int64_t peak_save_ts;  /* when saved_peak was last written            */
    double  current;       /* newest sample value, -1 = n/a               */
    double  threshold;     /* the limit in force when it started          */
    int     record;        /* index into g_rec[], -1 when idle            */
} alert_state_t;

/*
 * If two consecutive samples are further apart than this, the run is treated
 * as broken: we cannot claim the value stayed above the line while nothing
 * was being measured (a stop/restart, or a sampler that fell behind).
 */
#define ALERT_GAP_MAX_SEC 5

/* Peak is rewritten at most this often, so a long breach does not turn into
 * one disk write per second while still surviving a restart. */
#define ALERT_PEAK_SAVE_SEC 5

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static alert_thresholds_t  g_lim;
static alert_state_t     g_state[ALERT_METRIC_COUNT];
static int               g_available[ALERT_METRIC_COUNT];
static alert_record_t   g_rec[SLH_MAX_ALERTS];
static size_t           g_rec_count;      /* valid entries, newest last   */
static size_t           g_rec_head;       /* ring write index             */
static char             g_log_path[640];
static char             g_conf_path[640];
static uint64_t         g_disk_bytes;

/* ------------------------------------------------------------------ */
/* Naming                                                              */
/* ------------------------------------------------------------------ */

const char *alerts_metric_name(int metric)
{
    switch (metric) {
    case ALERT_METRIC_CPU:       return "CPU 使用率";
    case ALERT_METRIC_MEM:       return "内存使用率";
    case ALERT_METRIC_TEMP:      return "CPU 温度";
    /* panfrost 不报利用率，这里比的是"上电占比"，名字要如实 */
    case ALERT_METRIC_GPU:       return "GPU 活跃度";
    case ALERT_METRIC_NPU:       return "NPU 负载";
    case ALERT_METRIC_DISK_TEMP: return "硬盘温度";
    default: return "未知指标";
    }
}

/**
 * Whether the metric had a usable value at the last sample.
 *
 * A metric the kernel does not expose must not read as "0%, all fine": the
 * dashboard greys those rows out and says why instead.
 */
int alerts_metric_available(int metric)
{
    int v;

    if (metric < 0 || metric >= ALERT_METRIC_COUNT)
        return 0;
    pthread_mutex_lock(&g_lock);
    v = g_available[metric];
    pthread_mutex_unlock(&g_lock);
    return v;
}

static const char *metric_key(int metric)
{
    switch (metric) {
    case ALERT_METRIC_CPU:       return "cpu";
    case ALERT_METRIC_MEM:       return "mem";
    case ALERT_METRIC_TEMP:      return "temp";
    case ALERT_METRIC_GPU:       return "gpu";
    case ALERT_METRIC_NPU:       return "npu";
    case ALERT_METRIC_DISK_TEMP: return "disk_temp";
    default: return "?";
    }
}

static int metric_from_key(const char *key)
{
    if (strcmp(key, "cpu") == 0) return ALERT_METRIC_CPU;
    if (strcmp(key, "mem") == 0) return ALERT_METRIC_MEM;
    if (strcmp(key, "temp") == 0) return ALERT_METRIC_TEMP;
    if (strcmp(key, "gpu") == 0) return ALERT_METRIC_GPU;
    if (strcmp(key, "npu") == 0) return ALERT_METRIC_NPU;
    if (strcmp(key, "disk_temp") == 0) return ALERT_METRIC_DISK_TEMP;
    return -1;
}

/* Hysteresis margin: 5% of the limit, at least 2 units. */
static double margin_for(double threshold)
{
    double m = threshold * 0.05;

    return m < 2.0 ? 2.0 : m;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static void save_log_locked(void)
{
    FILE *fp;
    size_t i;

    if (g_log_path[0] == '\0')
        return;
    fp = fopen(g_log_path, "w");
    if (!fp) {
        LOG_WARN("cannot write %s: %s", g_log_path, strerror(errno));
        return;
    }
    fprintf(fp, "# epoch\tmetric\tpeak\tthreshold\tduration_sec\tended_ts\tinterrupted\n");
    for (i = 0; i < g_rec_count; i++) {
        /* Oldest first on disk, so a human reading the file sees it in order. */
        size_t idx = (g_rec_head + SLH_MAX_ALERTS - g_rec_count + i) % SLH_MAX_ALERTS;
        const alert_record_t *r = &g_rec[idx];

        fprintf(fp, "%lld\t%s\t%.1f\t%.1f\t%d\t%lld\t%d\n",
                (long long)r->ts, metric_key(r->metric), r->peak, r->threshold,
                r->duration_sec, (long long)r->ended_ts, r->interrupted);
    }
    if (fclose(fp) == 0) {
        struct stat st;

        if (stat(g_log_path, &st) == 0)
            g_disk_bytes = (uint64_t)st.st_size;
    }
}

static void save_thresholds_locked(void)
{
    FILE *fp;

    if (g_conf_path[0] == '\0')
        return;
    fp = fopen(g_conf_path, "w");
    if (!fp) {
        LOG_WARN("cannot write %s: %s", g_conf_path, strerror(errno));
        return;
    }
    fprintf(fp, "# Slhbond-Monitor 告警阈值（由面板写入，重启后生效）\n");
    fprintf(fp, "alert_cpu_percent = %.1f\n", g_lim.cpu_percent);
    fprintf(fp, "alert_mem_percent = %.1f\n", g_lim.mem_percent);
    fprintf(fp, "alert_temp_c = %.1f\n", g_lim.temp_c);
    fprintf(fp, "alert_gpu_percent = %.1f\n", g_lim.gpu_percent);
    fprintf(fp, "alert_npu_percent = %.1f\n", g_lim.npu_percent);
    fprintf(fp, "alert_disk_temp_c = %.1f\n", g_lim.disk_temp_c);
    fprintf(fp, "alert_hold_sec = %d\n", g_lim.hold_sec);
    fclose(fp);
}

static void load_thresholds_locked(void)
{
    FILE *fp;
    char  line[256];

    fp = fopen(g_conf_path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        char *key;
        char *val;

        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        key = slh_trim(line);
        val = slh_trim(eq + 1);

        if (strcmp(key, "alert_cpu_percent") == 0)
            g_lim.cpu_percent = strtod(val, NULL);
        else if (strcmp(key, "alert_mem_percent") == 0)
            g_lim.mem_percent = strtod(val, NULL);
        else if (strcmp(key, "alert_temp_c") == 0)
            g_lim.temp_c = strtod(val, NULL);
        else if (strcmp(key, "alert_gpu_percent") == 0)
            g_lim.gpu_percent = strtod(val, NULL);
        else if (strcmp(key, "alert_npu_percent") == 0)
            g_lim.npu_percent = strtod(val, NULL);
        else if (strcmp(key, "alert_disk_temp_c") == 0)
            g_lim.disk_temp_c = strtod(val, NULL);
        else if (strcmp(key, "alert_hold_sec") == 0)
            g_lim.hold_sec = (int)strtol(val, NULL, 10);
    }
    fclose(fp);
}

static void load_log_locked(void)
{
    FILE *fp;
    char  line[256];

    fp = fopen(g_log_path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char           *fields[7];
        int             n = 0;
        char           *p = line;
        alert_record_t  r;
        int             metric;

        if (line[0] == '#')
            continue;
        memset(&r, 0, sizeof(r));
        while (n < 7) {
            char *tab = strchr(p, '\t');

            fields[n++] = p;
            if (!tab)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        /* Older files have 6 columns; `interrupted` was added later. */
        if (n < 6)
            continue;
        fields[n - 1][strcspn(fields[n - 1], "\r\n")] = '\0';
        metric = metric_from_key(fields[1]);
        if (metric < 0)
            continue;
        r.ts = (int64_t)strtoll(fields[0], NULL, 10);
        r.metric = metric;
        r.peak = strtod(fields[2], NULL);
        r.threshold = strtod(fields[3], NULL);
        r.duration_sec = (int)strtol(fields[4], NULL, 10);
        r.ended_ts = (int64_t)strtoll(fields[5], NULL, 10);
        if (n >= 7)
            r.interrupted = (int)strtol(fields[6], NULL, 10);

        g_rec[g_rec_head] = r;
        g_rec_head = (g_rec_head + 1) % SLH_MAX_ALERTS;
        if (g_rec_count < SLH_MAX_ALERTS)
            g_rec_count++;
    }
    fclose(fp);
    if (g_rec_count)
        LOG_INFO("alerts: restored %zu record(s)", g_rec_count);
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                          */
/* ------------------------------------------------------------------ */

static void open_record_locked(int metric, int64_t ts, double peak,
                              double threshold)
{
    alert_record_t *r = &g_rec[g_rec_head];

    memset(r, 0, sizeof(*r));
    r->ts = ts;
    r->ended_ts = 0;
    r->metric = metric;
    r->interrupted = 0;
    r->peak = peak;
    r->threshold = threshold;
    r->duration_sec = 0;

    g_state[metric].record = (int)g_rec_head;
    g_state[metric].saved_peak = peak;
    g_state[metric].peak_save_ts = ts;
    g_rec_head = (g_rec_head + 1) % SLH_MAX_ALERTS;
    if (g_rec_count < SLH_MAX_ALERTS)
        g_rec_count++;

    LOG_INFO("alert: %s reached %.1f (limit %.1f)",
             alerts_metric_name(metric), peak, threshold);
    save_log_locked();
}

static void close_record_locked(int metric, int64_t ts, int interrupted)
{
    int idx = g_state[metric].record;

    if (idx < 0)
        return;
    g_rec[idx].ended_ts = ts;
    g_rec[idx].interrupted = interrupted;
    g_rec[idx].duration_sec = (int)(ts - g_rec[idx].ts);
    if (g_rec[idx].duration_sec < 0)
        g_rec[idx].duration_sec = 0;
    LOG_INFO("alert: %s %s after %d s (peak %.1f)",
             alerts_metric_name(metric),
             interrupted ? "interrupted" : "back to normal",
             g_rec[idx].duration_sec, g_rec[idx].peak);
    g_state[metric].record = -1;
    save_log_locked();
}

static void reset_run_locked(int metric)
{
    alert_state_t *st = &g_state[metric];

    st->above_since = 0;
    st->peak = 0.0;
}

static void evaluate_one_locked(int metric, double value, double threshold,
                                int64_t ts)
{
    alert_state_t *st = &g_state[metric];
    double         margin;

    /* A negative value means "this board does not report it", which is not
     * the same as zero and must never open or close a breach. */
    g_available[metric] = (value >= 0.0);
    st->current = value;
    if (value < 0.0) {
        reset_run_locked(metric);
        st->last_ts = ts;
        return;
    }

    margin = margin_for(threshold);

    /* A sampling gap means we did not actually observe the value staying
     * high, so start counting afresh rather than crediting the gap. */
    if (st->last_ts != 0 && ts - st->last_ts > ALERT_GAP_MAX_SEC)
        reset_run_locked(metric);
    st->last_ts = ts;

    if (!st->active) {
        if (value >= threshold) {
            if (st->above_since == 0) {
                st->above_since = ts;
                st->peak = 0.0;
                st->threshold = threshold;
            }
            if (st->peak < value)
                st->peak = value;

            /*
             * Compare wall-clock seconds, not sample counts: duplicated or
             * dropped samples must not change what "持续 10 秒" means.
             */
            if (ts - st->above_since >= g_lim.hold_sec) {
                st->active = 1;
                st->started_ts = st->above_since;
                st->threshold = threshold;
                open_record_locked(metric, st->above_since, st->peak, threshold);
            }
        } else {
            reset_run_locked(metric);
        }
        return;
    }

    /* Open record: keep peak and duration fresh. */
    if (st->peak < value)
        st->peak = value;
    if (st->record >= 0) {
        g_rec[st->record].peak = st->peak;
        g_rec[st->record].duration_sec = (int)(ts - st->started_ts);
        if (g_rec[st->record].duration_sec < 0)
            g_rec[st->record].duration_sec = 0;

        /* Flush the growing peak now and then so a restart cannot make the
         * record claim a lower peak than what actually happened. */
        if (st->peak > st->saved_peak &&
            ts - st->peak_save_ts >= ALERT_PEAK_SAVE_SEC) {
            st->saved_peak = st->peak;
            st->peak_save_ts = ts;
            save_log_locked();
        }
    }

    if (value < threshold - margin) {
        close_record_locked(metric, ts, 0);
        st->active = 0;
        reset_run_locked(metric);
    }
}

void alerts_evaluate(const hist_sample_t *s)
{
    int64_t ts = s->ts;

    pthread_mutex_lock(&g_lock);
    /* CPU is judged on the busiest single thread: an average can stay
     * comfortable while one core is pinned at 100%. */
    evaluate_one_locked(ALERT_METRIC_CPU, s->cpu_max >= 0.0 ? s->cpu_max : -1.0,
                        g_lim.cpu_percent, ts);
    evaluate_one_locked(ALERT_METRIC_MEM, s->mem_pct, g_lim.mem_percent, ts);
    evaluate_one_locked(ALERT_METRIC_TEMP, s->temp_c, g_lim.temp_c, ts);
    evaluate_one_locked(ALERT_METRIC_GPU, s->gpu_pct, g_lim.gpu_percent, ts);
    evaluate_one_locked(ALERT_METRIC_NPU, s->npu_pct, g_lim.npu_percent, ts);
    evaluate_one_locked(ALERT_METRIC_DISK_TEMP, s->disk_temp_c,
                        g_lim.disk_temp_c, ts);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int alerts_start(const config_t *cfg)
{
    int i;

    g_lim.cpu_percent = cfg->alert_cpu_percent;
    g_lim.mem_percent = cfg->alert_mem_percent;
    g_lim.temp_c = cfg->alert_temp_c;
    g_lim.gpu_percent = cfg->alert_gpu_percent;
    g_lim.npu_percent = cfg->alert_npu_percent;
    g_lim.disk_temp_c = cfg->alert_disk_temp_c;
    g_lim.hold_sec = cfg->alert_hold_sec;

    snprintf(g_log_path, sizeof(g_log_path), "%s/alerts.log", cfg->state_dir);
    snprintf(g_conf_path, sizeof(g_conf_path), "%s/alerts.conf", cfg->state_dir);

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < ALERT_METRIC_COUNT; i++) {
        g_state[i].record = -1;
        g_state[i].threshold = 0.0;
        g_state[i].current = -1.0;
    }
    /* A panel-written thresholds file wins over the packaged config. */
    load_thresholds_locked();
    load_log_locked();
    /*
     * Records restored with no end time were still running when we stopped,
     * so we cannot know whether they continued. End them where the evidence
     * ends and mark them as interrupted instead of pretending they ran on.
     */
    for (i = 0; i < (int)g_rec_count; i++) {
        size_t idx = (g_rec_head + SLH_MAX_ALERTS - g_rec_count + (size_t)i) % SLH_MAX_ALERTS;

        if (g_rec[idx].ended_ts == 0) {
            g_rec[idx].ended_ts = g_rec[idx].ts + g_rec[idx].duration_sec;
            g_rec[idx].interrupted = 1;
        }
    }
    save_log_locked();
    /* 把合并后的完整阈值写回去：老文件里没有的新指标会自动补上，
     * 用户打开这个文件就能看到当前生效的全部设置。 */
    save_thresholds_locked();
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("alerts: cpu>=%.0f%% mem>=%.0f%% cpuTemp>=%.0f°C gpu>=%.0f%% "
             "npu>=%.0f%% diskTemp>=%.0f°C hold=%ds",
             g_lim.cpu_percent, g_lim.mem_percent, g_lim.temp_c,
             g_lim.gpu_percent, g_lim.npu_percent, g_lim.disk_temp_c,
             g_lim.hold_sec);
    return 0;
}

void alerts_stop(void)
{
    int64_t now = slh_now_sec();
    int     m;

    pthread_mutex_lock(&g_lock);

    /*
     * A breach still building when we shut down would otherwise leave no
     * trace at all - which is exactly the "the CPU really did hit 100% but
     * nothing was recorded" case. Persist whatever we saw, clearly marked as
     * cut short by the stop rather than by the value coming back down.
     */
    for (m = 0; m < ALERT_METRIC_COUNT; m++) {
        alert_state_t *st = &g_state[m];
        int64_t        span;

        if (st->active) {
            close_record_locked(m, now, 1);
            st->active = 0;
            continue;
        }
        if (st->above_since == 0 || st->last_ts <= st->above_since)
            continue;
        span = st->last_ts - st->above_since;
        if (span < 2)              /* a single stray sample is not evidence */
            continue;
        open_record_locked(m, st->above_since, st->peak,
                           st->threshold > 0.0 ? st->threshold : 0.0);
        close_record_locked(m, st->last_ts, 1);
        st->above_since = 0;
        st->peak = 0.0;
    }

    save_log_locked();
    save_thresholds_locked();
    pthread_mutex_unlock(&g_lock);
}

size_t alerts_metric_states(alert_metric_state_t *out, size_t max)
{
    size_t n = 0;
    int    m;

    pthread_mutex_lock(&g_lock);
    for (m = 0; m < ALERT_METRIC_COUNT && n < max; m++) {
        const alert_state_t *st = &g_state[m];
        alert_metric_state_t *o = &out[n++];

        o->metric = m;
        o->available = g_available[m];
        o->active = st->active;
        o->current = st->current;
        o->threshold = (m == ALERT_METRIC_CPU) ? g_lim.cpu_percent
                     : (m == ALERT_METRIC_MEM) ? g_lim.mem_percent
                     : (m == ALERT_METRIC_TEMP) ? g_lim.temp_c
                     : (m == ALERT_METRIC_GPU) ? g_lim.gpu_percent
                     : (m == ALERT_METRIC_NPU) ? g_lim.npu_percent
                     : g_lim.disk_temp_c;
        o->pending_peak = st->peak;
        o->pending_sec = (st->above_since != 0 && st->last_ts > st->above_since)
            ? (int)(st->last_ts - st->above_since) : 0;
        if (st->active && st->record >= 0) {
            /* 已经开记录：显示已持续多久 */
            int d = g_rec[st->record].duration_sec;

            if (d > o->pending_sec)
                o->pending_sec = d;
        }
        o->hold_sec = g_lim.hold_sec;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

size_t alerts_list(alert_record_t *out, size_t max)
{
    size_t taken = 0;
    size_t i;

    pthread_mutex_lock(&g_lock);
    /* Newest first: walk the ring backwards from head. */
    for (i = 0; i < g_rec_count && taken < max; i++) {
        size_t idx = (g_rec_head + SLH_MAX_ALERTS - 1 - i) % SLH_MAX_ALERTS;

        out[taken++] = g_rec[idx];
    }
    pthread_mutex_unlock(&g_lock);
    return taken;
}

int alerts_active_count(void)
{
    int n = 0;
    int i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < ALERT_METRIC_COUNT; i++)
        n += g_state[i].active ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    return n;
}

size_t alerts_total(void)
{
    size_t n;

    pthread_mutex_lock(&g_lock);
    n = g_rec_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t alerts_disk_bytes(void)
{
    uint64_t n;

    pthread_mutex_lock(&g_lock);
    n = g_disk_bytes;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void alerts_get_thresholds(alert_thresholds_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_lim;
    pthread_mutex_unlock(&g_lock);
}

int alerts_set_thresholds(const alert_thresholds_t *in, char *err, size_t err_size)
{
    static const struct {
        double value;
        double lo;
        double hi;
        const char *what;
    } pct[] = {
        { 0, 1.0, 100.0, "CPU 阈值" },
        { 0, 1.0, 100.0, "内存阈值" },
        { 0, 1.0, 100.0, "GPU 阈值" },
        { 0, 1.0, 100.0, "NPU 阈值" },
    };
    double vals[4];
    int    i;

    vals[0] = in->cpu_percent;
    vals[1] = in->mem_percent;
    vals[2] = in->gpu_percent;
    vals[3] = in->npu_percent;

    for (i = 0; i < 4; i++) {
        if (vals[i] < pct[i].lo || vals[i] > pct[i].hi) {
            snprintf(err, err_size, "%s必须在 %.0f-%.0f 之间",
                     pct[i].what, pct[i].lo, pct[i].hi);
            return -1;
        }
    }
    if (in->temp_c < 20.0 || in->temp_c > 150.0) {
        snprintf(err, err_size, "CPU 温度阈值必须在 20-150 之间");
        return -1;
    }
    if (in->disk_temp_c < 20.0 || in->disk_temp_c > 100.0) {
        snprintf(err, err_size, "硬盘温度阈值必须在 20-100 之间");
        return -1;
    }
    if (in->hold_sec < 1 || in->hold_sec > 3600) {
        snprintf(err, err_size, "持续时间必须在 1-3600 秒之间");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_lim = *in;
    save_thresholds_locked();
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("alerts: thresholds updated - cpu>=%.0f%% mem>=%.0f%% cpuTemp>=%.0f°C "
             "gpu>=%.0f%% npu>=%.0f%% diskTemp>=%.0f°C hold=%ds",
             in->cpu_percent, in->mem_percent, in->temp_c,
             in->gpu_percent, in->npu_percent, in->disk_temp_c, in->hold_sec);
    return 0;
}
