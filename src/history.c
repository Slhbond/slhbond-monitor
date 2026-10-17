/*
 * history.c - compact per-second hardware time series on local disk.
 *
 * See history.h for the design rationale. The short version:
 *   - one fixed-size binary record per second (32 B + 1 B per core);
 *   - a preallocated ring file, so growth is bounded by configuration;
 *   - records are staged in memory and flushed in batches, so the flash is
 *     written once per history_flush_sec rather than once per second.
 */
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "alerts.h"
#include "cmdlog.h"
#include "events.h"
#include "log.h"
#include "util.h"

#define HIST_MAGIC      "SLHHIST1"
#define HIST_HDR_SIZE   48
#define HIST_REC_FIXED  40
#define HIST_BATCH_MAX  600          /* cap on staged records (~10 min)     */

/* How often the sampler also re-reads the login database (in ticks/seconds). */
#define EVENT_POLL_TICKS 10

/*
 * The /proc scan runs several times a second rather than once: a process has
 * to be alive at the moment of a scan to be seen at all, so the scan rate is
 * the hard limit on how short a command can be caught. (Interactive commands
 * that slip through are picked up from shell history instead.)
 */
#define PROC_SCAN_PER_SEC 4

/* ------------------------------------------------------------------ */
/* On-disk layout                                                      */
/* ------------------------------------------------------------------ */

/*
 * Both structs are written verbatim, so their layout is part of the file
 * format. The static asserts below stop a compiler from silently inserting
 * padding that would make old files unreadable.
 */
typedef struct {
    uint32_t ts;                    /* epoch seconds                     */
    uint16_t temp_c10;              /* tenths of °C, 0xFFFF = n/a        */
    uint16_t freq_mhz;              /* 0 = n/a                           */
    uint16_t gpu_temp_c10;          /* tenths of °C, 0xFFFF = n/a        */
    uint16_t gpu_freq_mhz;          /* 0 = n/a                           */
    uint16_t disk_temp_c10;         /* hottest disk, tenths of °C        */
    uint16_t reserved;              /* keeps the struct padding-free     */
    uint8_t  cpu_avg;               /* 0..254, 255 = n/a                 */
    uint8_t  cpu_max;
    uint8_t  mem_pct;
    uint8_t  gpu_pct;
    uint8_t  npu_pct;
    uint8_t  flags;                 /* bit0: an alert was active         */
    uint16_t load1_x100;            /* load average * 100, saturating    */
    uint32_t net_rx_bps;
    uint32_t net_tx_bps;
    uint32_t disk_r_bps;
    uint32_t disk_w_bps;
} hist_packed_t;

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t rec_size;              /* HIST_REC_FIXED + ncores           */
    uint32_t ncores;
    uint32_t capacity;              /* records the ring can hold         */
    uint32_t count;                 /* valid records, <= capacity        */
    uint32_t head;                  /* ring index of the next write      */
    uint32_t interval_sec;
    uint32_t reserved[3];
} hist_file_hdr_t;

_Static_assert(sizeof(hist_packed_t) == HIST_REC_FIXED,
               "packed record must stay 40 bytes");
_Static_assert(sizeof(hist_file_hdr_t) == HIST_HDR_SIZE,
               "file header must stay 48 bytes");

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_thread;
static int             g_thread_started;
static volatile int    g_stop;

static int             g_fd = -1;
static char            g_path[640];
static hist_file_hdr_t g_hdr;
static uint32_t        g_rec_size;

/* Staged records, laid out exactly as they will be written. */
static uint8_t         g_batch[HIST_BATCH_MAX * (HIST_REC_FIXED + SLH_HIST_MAX_CORES)];
static size_t          g_batch_len;

static hist_source_fn  g_src;
static void           *g_src_user;

/* ------------------------------------------------------------------ */
/* Value packing                                                       */
/* ------------------------------------------------------------------ */

static uint8_t pack_pct(double v)
{
    if (v < 0.0)
        return SLH_HIST_NA_U8;
    if (v > 254.0)
        return 254u;                 /* 255 is reserved for "n/a" */
    return (uint8_t)(v + 0.5);
}

static uint16_t pack_temp(double celsius)
{
    if (celsius < 0.0)
        return SLH_HIST_NA_U16;
    if (celsius > 650.0)
        return 6500u;
    return (uint16_t)(celsius * 10.0 + 0.5);
}

static uint32_t pack_rate(double bps)
{
    if (!(bps > 0.0))
        return 0u;
    if (bps > 4294967295.0)
        return 4294967295u;
    return (uint32_t)(bps + 0.5);
}

static uint16_t pack_load(double load1)
{
    double v = load1 * 100.0;

    if (!(v > 0.0))
        return 0u;
    if (v > 65535.0)
        return 65535u;
    return (uint16_t)(v + 0.5);
}

static int unpack_pct(uint8_t v)
{
    return v == SLH_HIST_NA_U8 ? -1 : (int)v;
}

/* ------------------------------------------------------------------ */
/* Ring file                                                           */
/* ------------------------------------------------------------------ */

static int write_header(void)
{
    ssize_t n = pwrite(g_fd, &g_hdr, sizeof(g_hdr), 0);

    return n == (ssize_t)sizeof(g_hdr) ? 0 : -1;
}

/**
 * Open (or create) the ring file for `ncores` cores and `capacity` records.
 * A file written for a different core count is rebuilt: the record size
 * differs, so the old contents cannot be interpreted.
 */
static int ring_open(const char *path, int ncores, uint32_t capacity)
{
    uint32_t rec_size = (uint32_t)HIST_REC_FIXED + (uint32_t)ncores;
    off_t    want = (off_t)HIST_HDR_SIZE + (off_t)capacity * (off_t)rec_size;
    hist_file_hdr_t hdr;
    int      fd;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        ssize_t n = pread(fd, &hdr, sizeof(hdr), 0);

        if (n == (ssize_t)sizeof(hdr) && memcmp(hdr.magic, HIST_MAGIC, 8) == 0 &&
            hdr.version == 3 && hdr.rec_size == rec_size &&
            hdr.capacity == capacity && hdr.ncores == (uint32_t)ncores) {
            g_hdr = hdr;
            g_rec_size = rec_size;
            g_fd = fd;
            return 0;
        }
        LOG_WARN("history file %s is incompatible - rebuilding it", path);
        close(fd);
        unlink(path);
    } else if (errno != ENOENT) {
        LOG_WARN("cannot open history file %s: %s", path, strerror(errno));
        return -1;
    }

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        LOG_WARN("cannot create history file %s: %s", path, strerror(errno));
        return -1;
    }
    /* Preallocate once so the ring never grows and never fragments. */
    if (ftruncate(fd, want) != 0) {
        LOG_WARN("cannot size history file %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&g_hdr, 0, sizeof(g_hdr));
    memcpy(g_hdr.magic, HIST_MAGIC, 8);
    g_hdr.version = 3;
    g_hdr.rec_size = rec_size;
    g_hdr.ncores = (uint32_t)ncores;
    g_hdr.capacity = capacity;
    g_hdr.count = 0;
    g_hdr.head = 0;
    g_hdr.interval_sec = 1;
    g_rec_size = rec_size;
    g_fd = fd;

    if (write_header() != 0) {
        LOG_WARN("cannot write history header: %s", strerror(errno));
        close(fd);
        g_fd = -1;
        return -1;
    }
    LOG_INFO("history: %u records x %u B = %.1f MB ring at %s",
             capacity, rec_size,
             ((double)want) / 1048576.0, path);
    return 0;
}

/**
 * Write the staged records into the ring and publish the new header.
 *
 * The run may straddle the end of the ring, so it is written as at most two
 * contiguous chunks.
 */
static void ring_flush_locked(void)
{
    uint32_t pos;
    size_t   done = 0;

    if (g_fd < 0 || g_batch_len == 0)
        return;

    pos = g_hdr.head;
    while (done < g_batch_len) {
        uint32_t room = g_hdr.capacity - pos;
        size_t   run = g_batch_len - done;

        if ((size_t)room < run)
            run = room;

        if (pwrite(g_fd, g_batch + done * g_rec_size, run * g_rec_size,
                   (off_t)HIST_HDR_SIZE + (off_t)pos * (off_t)g_rec_size)
            != (ssize_t)(run * g_rec_size)) {
            LOG_WARN("history write failed: %s", strerror(errno));
            break;
        }
        pos = (uint32_t)((pos + run) % g_hdr.capacity);
        done += run;
    }

    g_hdr.head = pos;
    if (g_hdr.count + g_batch_len >= g_hdr.capacity)
        g_hdr.count = g_hdr.capacity;
    else
        g_hdr.count += (uint32_t)g_batch_len;

    if (write_header() != 0)
        LOG_WARN("history header write failed: %s", strerror(errno));

    g_batch_len = 0;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

static void history_append(const hist_sample_t *s)
{
    hist_packed_t rec;
    int           n = s->ncores;
    int           i;

    /*
     * Never write more core bytes than the record has room for. The ring is
     * sized once, from the core count seen at start-up, but CPU hotplug can
     * change that at runtime - clamping here turns "record is short" into a
     * missing column instead of a heap overflow.
     */
    {
        int room = (int)g_rec_size - HIST_REC_FIXED;

        if (room < 0)
            room = 0;
        if (n > room)
            n = room;
    }
    if (n > SLH_HIST_MAX_CORES)
        n = SLH_HIST_MAX_CORES;
    if (n < 0)
        n = 0;

    memset(&rec, 0, sizeof(rec));
    rec.ts = (uint32_t)s->ts;
    rec.temp_c10 = pack_temp(s->temp_c);
    rec.freq_mhz = (s->freq_mhz > 0.0)
        ? (uint16_t)(s->freq_mhz > 65535.0 ? 65535.0 : s->freq_mhz + 0.5) : 0u;
    rec.cpu_avg = pack_pct(s->cpu_avg);
    rec.cpu_max = pack_pct(s->cpu_max);
    rec.mem_pct = pack_pct(s->mem_pct);
    rec.gpu_pct = pack_pct(s->gpu_pct);
    rec.npu_pct = pack_pct(s->npu_pct);
    rec.gpu_temp_c10 = pack_temp(s->gpu_temp_c);
    rec.gpu_freq_mhz = (s->gpu_freq_mhz > 0.0)
        ? (uint16_t)(s->gpu_freq_mhz > 65535.0 ? 65535.0 : s->gpu_freq_mhz + 0.5) : 0u;
    rec.disk_temp_c10 = pack_temp(s->disk_temp_c);
    rec.reserved = 0;
    rec.flags = 0;
    rec.load1_x100 = pack_load(s->load1);
    rec.net_rx_bps = pack_rate(s->net_rx_bps);
    rec.net_tx_bps = pack_rate(s->net_tx_bps);
    rec.disk_r_bps = pack_rate(s->disk_r_bps);
    rec.disk_w_bps = pack_rate(s->disk_w_bps);

    pthread_mutex_lock(&g_lock);
    if (g_fd >= 0) {
        uint8_t *slot;

        if (g_batch_len >= HIST_BATCH_MAX)
            ring_flush_locked();

        slot = g_batch + g_batch_len * g_rec_size;
        memcpy(slot, &rec, HIST_REC_FIXED);
        for (i = 0; i < n; i++)
            slot[HIST_REC_FIXED + i] = pack_pct(s->core[i]);
        memset(slot + HIST_REC_FIXED + n, SLH_HIST_NA_U8,
               g_rec_size - HIST_REC_FIXED - (size_t)n);
        g_batch_len++;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Sampler thread                                                      */
/* ------------------------------------------------------------------ */

/**
 * Sleep until the next whole second of wall-clock time, so samples land on a
 * stable 1 Hz grid instead of drifting with however long each tick took.
 *
 * Note this computes a *duration*: nanosleep(2) takes a relative interval, so
 * passing the absolute target time would ask it to sleep for ~56 years.
 */
static void sleep_to_next_tick(int64_t tick_sec)
{
    struct timespec now;
    struct timespec dur;
    int64_t         now_sec;
    int64_t         next_sec;

    if (tick_sec < 1)
        tick_sec = 1;

    clock_gettime(CLOCK_REALTIME, &now);
    now_sec = (int64_t)now.tv_sec;
    next_sec = (now_sec / tick_sec + 1) * tick_sec;

    dur.tv_sec = (time_t)(next_sec - now_sec - 1);
    dur.tv_nsec = 1000000000L - now.tv_nsec;
    if (dur.tv_nsec >= 1000000000L) {
        dur.tv_nsec -= 1000000000L;
        dur.tv_sec += 1;
    }
    if (dur.tv_sec < 0)
        dur.tv_sec = 0;
    if (dur.tv_nsec < 0)
        dur.tv_nsec = 0;

    while (nanosleep(&dur, &dur) != 0 && errno == EINTR)
        continue;
}

static void msleep(int64_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        continue;
}

static void *sampler_main(void *arg)
{
    int64_t tick = (int64_t)(intptr_t)arg;
    int     since_poll = 0;

    if (tick < 1)
        tick = 1;

    while (!g_stop) {
        hist_sample_t s;
        int           sub;

        /* One full hardware sample per tick. */
        memset(&s, 0, sizeof(s));
        if (g_src && g_src(g_src_user, &s) == 0) {
            history_append(&s);
            alerts_evaluate(&s);
        }

        /*
         * The utmp database is append-only, so re-reading it is cheap and only
         * picks up new records. Doing it here (rather than only at start-up)
         * is what makes a login show up while the service is running instead
         * of after the next restart.
         */
        if (++since_poll >= EVENT_POLL_TICKS) {
            since_poll = 0;
            events_poll();
        }

        /*
         * Command capture between ticks: sub-second polling is what lets a
         * short-lived delete show up. It writes to disk only when something
         * new appears, so the extra scans cost no flash wear.
         */
        for (sub = 0; sub < PROC_SCAN_PER_SEC && !g_stop; sub++) {
            cmdlog_poll();
            if (sub + 1 < PROC_SCAN_PER_SEC)
                msleep(1000 / PROC_SCAN_PER_SEC);
        }

        sleep_to_next_tick(tick);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int history_start(const config_t *cfg, hist_source_fn src, void *user)
{
    char          path[640];
    hist_sample_t probe;
    int           have_probe = 0;
    int           ncores;
    uint32_t      capacity;

    if (!cfg->history_enabled) {
        LOG_INFO("history disabled by configuration");
        return 0;
    }

    /*
     * Take one real sample before sizing the file: the record length depends
     * on the core count, and guessing it wrong here is what makes the ring
     * and the samples disagree. The probe is appended below rather than
     * thrown away, so it also seeds the trace view immediately.
     */
    memset(&probe, 0, sizeof(probe));
    if (src && src(user, &probe) == 0)
        have_probe = 1;

    ncores = cfg->history_cores;
    if (ncores < 1)
        ncores = have_probe ? probe.ncores : 1;
    if (ncores < 1)
        ncores = 1;
    if (ncores > SLH_HIST_MAX_CORES)
        ncores = SLH_HIST_MAX_CORES;

    capacity = (uint32_t)cfg->history_hours * 3600u;
    if (capacity < 60u)
        capacity = 60u;

    snprintf(path, sizeof(path), "%s/history.bin", cfg->state_dir);
    slh_strlcpy(g_path, path, sizeof(g_path));

    if (ring_open(path, ncores, capacity) != 0)
        return -1;

    g_src = src;
    g_src_user = user;
    g_stop = 0;

    if (have_probe) {
        history_append(&probe);
        alerts_evaluate(&probe);
    }

    if (pthread_create(&g_thread, NULL, sampler_main,
                       (void *)(intptr_t)cfg->history_interval_sec) != 0) {
        LOG_WARN("cannot start the history sampler: %s", strerror(errno));
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    g_thread_started = 1;
    LOG_INFO("history sampler started (%us per sample, %d core(s), %u h retention)",
             (unsigned)cfg->history_interval_sec, ncores, (unsigned)cfg->history_hours);
    return 0;
}

void history_stop(void)
{
    if (!g_thread_started)
        return;

    g_stop = 1;
    pthread_join(g_thread, NULL);
    g_thread_started = 0;

    pthread_mutex_lock(&g_lock);
    ring_flush_locked();
    if (g_fd >= 0) {
        /* Make the newest window durable before the process goes away. */
        if (fdatasync(g_fd) != 0)
            LOG_DEBUG("history fdatasync: %s", strerror(errno));
        close(g_fd);
        g_fd = -1;
    }
    pthread_mutex_unlock(&g_lock);
    LOG_INFO("history stopped");
}

int history_ready(void)
{
    int ready;

    pthread_mutex_lock(&g_lock);
    ready = g_fd >= 0 && g_hdr.count > 0;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

/**
 * Records that are queryable: what is on disk plus what is still staged in
 * memory. The staged part must be included or the newest
 * `history_flush_sec` worth of samples would be invisible - which is the
 * whole window right after start-up.
 */
static uint32_t visible_count_locked(void)
{
    if (g_fd < 0)
        return 0;
    return g_hdr.count + (uint32_t)g_batch_len;
}

void history_range(int64_t *from, int64_t *to)
{
    pthread_mutex_lock(&g_lock);
    if (g_fd < 0 || visible_count_locked() == 0) {
        if (from) *from = 0;
        if (to) *to = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    {
        hist_packed_t rec;

        if (from) {
            *from = 0;
            if (g_hdr.count > 0) {
                uint32_t oldest = (g_hdr.head + g_hdr.capacity - g_hdr.count)
                                  % g_hdr.capacity;

                if (pread(g_fd, &rec, HIST_REC_FIXED,
                          (off_t)HIST_HDR_SIZE + (off_t)oldest * (off_t)g_rec_size)
                    == (ssize_t)HIST_REC_FIXED)
                    *from = (int64_t)rec.ts;
            }
            if (*from == 0 && g_batch_len > 0) {
                memcpy(&rec, g_batch, HIST_REC_FIXED);
                *from = (int64_t)rec.ts;
            }
        }
        if (to) {
            *to = 0;
            if (g_batch_len > 0) {
                memcpy(&rec, g_batch + (g_batch_len - 1) * g_rec_size,
                       HIST_REC_FIXED);
                *to = (int64_t)rec.ts;
            } else if (g_hdr.count > 0) {
                uint32_t newest = (g_hdr.head + g_hdr.capacity - 1u) % g_hdr.capacity;

                if (pread(g_fd, &rec, HIST_REC_FIXED,
                          (off_t)HIST_HDR_SIZE + (off_t)newest * (off_t)g_rec_size)
                    == (ssize_t)HIST_REC_FIXED)
                    *to = (int64_t)rec.ts;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void unpack_record(const uint8_t *raw, hist_record_t *out, int ncores)
{
    hist_packed_t rec;
    int           i;

    memcpy(&rec, raw, HIST_REC_FIXED);
    out->ts = (int64_t)rec.ts;
    out->cpu_avg = unpack_pct(rec.cpu_avg);
    out->cpu_max = unpack_pct(rec.cpu_max);
    out->mem_pct = unpack_pct(rec.mem_pct);
    out->gpu_pct = unpack_pct(rec.gpu_pct);
    out->npu_pct = unpack_pct(rec.npu_pct);
    out->gpu_temp_c10 = rec.gpu_temp_c10 == SLH_HIST_NA_U16 ? -1 : (int)rec.gpu_temp_c10;
    out->gpu_freq_mhz = (int)rec.gpu_freq_mhz;
    out->disk_temp_c10 = rec.disk_temp_c10 == SLH_HIST_NA_U16
        ? -1 : (int)rec.disk_temp_c10;
    out->temp_c10 = rec.temp_c10 == SLH_HIST_NA_U16 ? -1 : (int)rec.temp_c10;
    out->freq_mhz = (int)rec.freq_mhz;
    out->load1_x100 = (int)rec.load1_x100;
    out->net_rx_bps = rec.net_rx_bps;
    out->net_tx_bps = rec.net_tx_bps;
    out->disk_r_bps = rec.disk_r_bps;
    out->disk_w_bps = rec.disk_w_bps;
    out->ncores = ncores;
    for (i = 0; i < ncores; i++)
        out->core[i] = unpack_pct(raw[HIST_REC_FIXED + i]);
    for (; i < SLH_HIST_MAX_CORES; i++)
        out->core[i] = -1;
}

size_t history_query(int64_t from, int64_t to, hist_record_t *out, size_t max,
                     int *decimated)
{
    uint8_t *raw = NULL;
    size_t   taken = 0;
    uint32_t count;
    uint32_t total;
    uint32_t first;
    uint32_t step = 1;
    int      ncores;
    uint32_t i;

    if (decimated)
        *decimated = 0;

    pthread_mutex_lock(&g_lock);
    if (g_fd < 0 || max == 0 || visible_count_locked() == 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    count = g_hdr.count;
    total = visible_count_locked();
    ncores = (int)g_hdr.ncores;
    first = (g_hdr.head + g_hdr.capacity - count) % g_hdr.capacity;

    /*
     * Materialise the visible window: the on-disk ring first, then the staged
     * batch, which logically continues right after it. `total` is bounded by
     * the configured capacity, so this is a few MB at most.
     */
    raw = xmalloc((size_t)total * g_rec_size);

    if (count > 0) {
        uint32_t run = g_hdr.capacity - first;

        if (run > count)
            run = count;
        if (pread(g_fd, raw, (size_t)run * g_rec_size,
                  (off_t)HIST_HDR_SIZE + (off_t)first * (off_t)g_rec_size)
            != (ssize_t)((size_t)run * g_rec_size)) {
            free(raw);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        if (run < count) {
            if (pread(g_fd, raw + (size_t)run * g_rec_size,
                      (size_t)(count - run) * g_rec_size,
                      (off_t)HIST_HDR_SIZE) !=
                (ssize_t)((size_t)(count - run) * g_rec_size)) {
                free(raw);
                pthread_mutex_unlock(&g_lock);
                return 0;
            }
        }
    }
    if (g_batch_len > 0)
        memcpy(raw + (size_t)count * g_rec_size, g_batch,
               g_batch_len * g_rec_size);

    /* Pass 1: how many records fall inside the window? */
    {
        uint32_t hits = 0;

        for (i = 0; i < total; i++) {
            hist_packed_t rec;

            memcpy(&rec, raw + (size_t)i * g_rec_size, HIST_REC_FIXED);
            if (from && (int64_t)rec.ts < from)
                continue;
            if (to && (int64_t)rec.ts > to)
                continue;
            hits++;
        }
        if (hits == 0) {
            free(raw);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        if (hits > max) {
            step = hits / (uint32_t)max;
            if (step < 1)
                step = 1;
            if (decimated)
                *decimated = 1;
        }
    }

    /* Pass 2: copy, decimating evenly so the whole span stays visible. */
    {
        uint32_t seen = 0;

        for (i = 0; i < total && taken < max; i++) {
            hist_packed_t rec;

            memcpy(&rec, raw + (size_t)i * g_rec_size, HIST_REC_FIXED);
            if (from && (int64_t)rec.ts < from)
                continue;
            if (to && (int64_t)rec.ts > to)
                continue;
            if (seen % step == 0)
                unpack_record(raw + (size_t)i * g_rec_size, &out[taken++], ncores);
            seen++;
        }
    }

    free(raw);
    pthread_mutex_unlock(&g_lock);
    return taken;
}

void history_stats(size_t *count, size_t *capacity, int *interval_sec)
{
    pthread_mutex_lock(&g_lock);
    if (count) *count = (size_t)visible_count_locked();
    if (capacity) *capacity = g_fd >= 0 ? (size_t)g_hdr.capacity : 0;
    if (interval_sec) *interval_sec = g_fd >= 0 ? (int)g_hdr.interval_sec : 0;
    pthread_mutex_unlock(&g_lock);
}

uint64_t history_disk_bytes(void)
{
    uint64_t total;

    pthread_mutex_lock(&g_lock);
    total = g_fd >= 0
        ? (uint64_t)HIST_HDR_SIZE + (uint64_t)g_hdr.capacity * g_rec_size : 0;
    pthread_mutex_unlock(&g_lock);
    return total;
}
