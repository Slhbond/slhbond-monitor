/*
 * disk.c - block-device enumeration and health checking.
 *
 * Health sources, tried in order of authority and combined by taking the
 * worst result:
 *
 *   1. eMMC/SD  - the vendor's own wear counters in sysfs
 *                 (life_time, pre_eol_info). Available to any user, no
 *                 subprocess, and genuinely authoritative.
 *   2. ATA/NVMe - smartctl, when the binary is installed. Run through
 *                 slh_run_command() with a hard timeout.
 *   3. Fallback - filesystem occupancy / read-only remount, which catches a
 *                 full or remounted-read-only volume even when nothing else
 *                 can be read.
 *
 * Nothing here fabricates a verdict: a device that yields no data at all is
 * reported as UNKNOWN rather than "healthy".
 */
#include "disk.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "json.h"
#include "log.h"

#define SYS_BLOCK     "/sys/block"
#define PROC_MOUNTS   "/proc/mounts"

#define CHECK_QUEUE_LEN 8
#define SMARTCTL_TIMEOUT_MS 20000

/*
 * disk_collect() is the public entry point; internally the health worker
 * needs the same enumeration *without* touching the I/O rate cache, so both
 * go through this one implementation.
 */
static void disk_collect_internal(disk_list_t *out, int sample_io);

/* ------------------------------------------------------------------ */
/* Health result cache                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char          device[32];
    int           valid;
    disk_health_t health;
    char          source[40];
    char          summary[192];
    char          detail[768];
    char          checked_at[40];
    int           auto_checked;
    double        temperature_c;
    uint64_t      power_on_hours;
    /* Identity fields smartctl reports better than sysfs does; merged into
     * the enumeration when sysfs has nothing. */
    char          model[96];
    char          serial[64];
    char          firmware[64];
} health_record_t;

static pthread_mutex_t g_health_lock = PTHREAD_MUTEX_INITIALIZER;
static health_record_t g_health[SLH_MAX_DISKS];
static size_t          g_health_count;

static health_record_t *health_lookup(const char *device, int create)
{
    size_t i;

    for (i = 0; i < g_health_count; i++) {
        if (strcmp(g_health[i].device, device) == 0)
            return &g_health[i];
    }
    if (!create || g_health_count >= SLH_MAX_DISKS)
        return NULL;

    memset(&g_health[g_health_count], 0, sizeof(g_health[0]));
    slh_strlcpy(g_health[g_health_count].device, device,
                sizeof(g_health[0].device));
    g_health[g_health_count].temperature_c = -1.0;
    return &g_health[g_health_count++];
}

/* ------------------------------------------------------------------ */
/* Health result persistence                                           */
/* ------------------------------------------------------------------ */

/*
 * Results are cached in memory, so without this a restart would blank every
 * verdict - and because the device is already recorded as "auto-checked", it
 * would never be re-checked either, leaving the card stuck on 未检测 forever.
 * A small tab-separated file (newlines/tabs escaped) keeps the last known
 * state across restarts without pulling in a config parser.
 */

#define HEALTH_STATE_FILE "disk-health.state"
#define AUTOCHECK_FILE    "disk-autocheck"

/*
 * The writable state *directory*, filled in by disk_init(). Both persistent
 * files are built from it by state_file_path() - keeping one directory
 * variable avoids the classic mistake of storing a file path in something
 * that later gets treated as a directory.
 */
static char g_state_dir[512];

static void state_file_path(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    if (!g_state_dir[0]) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%s/%s", g_state_dir, name);
}

static void escape_field(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (; in && *in && o + 2 < out_size; in++) {
        switch (*in) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:   out[o++] = *in;                   break;
        }
    }
    out[o] = '\0';
}

static void unescape_field(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (; in && *in && o + 1 < out_size; in++) {
        if (*in == '\\' && in[1]) {
            in++;
            if (*in == 'n')
                out[o++] = '\n';
            else if (*in == 't')
                out[o++] = '\t';
            else
                out[o++] = *in;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

/** Caller must hold g_health_lock. */
static void health_save_locked(void)
{
    char   path[640];
    FILE  *fp;
    size_t i;

    state_file_path(HEALTH_STATE_FILE, path, sizeof(path));
    if (!path[0])
        return;
    fp = fopen(path, "w");
    if (!fp) {
        LOG_DEBUG("cannot write %s", path);
        return;
    }

    for (i = 0; i < g_health_count; i++) {
        const health_record_t *r = &g_health[i];
        char summary[400], detail[1700], serial[160], model[220], fw[160];

        if (!r->valid)
            continue;
        escape_field(r->summary, summary, sizeof(summary));
        escape_field(r->detail, detail, sizeof(detail));
        escape_field(r->serial, serial, sizeof(serial));
        escape_field(r->model, model, sizeof(model));
        escape_field(r->firmware, fw, sizeof(fw));

        fprintf(fp, "%s\t%d\t%s\t%d\t%.2f\t%llu\t%s\t%s\t%s\t%s\t%s\t%s\n",
                r->device, (int)r->health, r->source, r->auto_checked,
                r->temperature_c, (unsigned long long)r->power_on_hours,
                r->checked_at, summary, serial, model, fw, detail);
    }
    fclose(fp);
}

/**
 * Split a tab-separated state line into fields, keeping empty ones.
 *
 * strtok_r(3) collapses runs of delimiters, so a record with an empty field
 * (a drive whose serial is unknown, say) would silently lose a column and be
 * discarded. This splitter treats every tab as exactly one separator.
 *
 * Returns the number of fields found.
 */
static int split_tabs(char *line, char **fields, int max_fields)
{
    size_t len = strlen(line);
    char  *p = line;
    int    count = 0;

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    fields[count++] = p;
    while (*p && count < max_fields) {
        if (*p == '\t') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

static void health_load(void)
{
    char   path[640];
    FILE  *fp;
    char   line[4096];

    state_file_path(HEALTH_STATE_FILE, path, sizeof(path));
    if (!path[0])
        return;
    fp = fopen(path, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char           *fields[13];
        int             count;
        health_record_t rec;

        memset(&rec, 0, sizeof(rec));
        rec.temperature_c = -1.0;

        count = split_tabs(line, fields, 13);
        /*
         * Accept anything from 12 fields up. The current writer emits 12, but
         * an earlier build wrote 13 (a since-removed wear column); dropping
         * such a record would leave the device stuck on "未检测" forever,
         * because the auto-check record already says it has been checked.
         * Extra trailing fields are simply ignored.
         */
        if (count < 12) {
            LOG_DEBUG("skipping malformed health record (%d field(s))", count);
            continue;
        }

        slh_strlcpy(rec.device, fields[0], sizeof(rec.device));
        rec.health = (disk_health_t)atoi(fields[1]);
        slh_strlcpy(rec.source, fields[2], sizeof(rec.source));
        rec.auto_checked = atoi(fields[3]);
        rec.temperature_c = strtod(fields[4], NULL);
        rec.power_on_hours = strtoull(fields[5], NULL, 10);
        slh_strlcpy(rec.checked_at, fields[6], sizeof(rec.checked_at));
        unescape_field(fields[7], rec.summary, sizeof(rec.summary));
        unescape_field(fields[8], rec.serial, sizeof(rec.serial));
        unescape_field(fields[9], rec.model, sizeof(rec.model));
        unescape_field(fields[10], rec.firmware, sizeof(rec.firmware));
        unescape_field(fields[11], rec.detail, sizeof(rec.detail));
        rec.valid = 1;

        pthread_mutex_lock(&g_health_lock);
        {
            health_record_t *slot = health_lookup(rec.device, 1);

            if (slot)
                *slot = rec;
        }
        pthread_mutex_unlock(&g_health_lock);
    }
    fclose(fp);
    LOG_DEBUG("restored %zu cached disk health record(s)", g_health_count);
}

/* ------------------------------------------------------------------ */
/* Previous I/O counters (for the throughput read-out)                 */
/* ------------------------------------------------------------------ */

/*
 * /sys/block/X/stat holds cumulative sector counts, so a rate needs two
 * readings. The previous sample is cached per device behind one mutex.
 *
 * disk_collect() fills this in; the health worker deliberately does not
 * (it passes sample_io = 0), otherwise a health check running between two
 * polls would shrink the sampling window and produce a bogus spike.
 */
#define SLH_MAX_IO_TRACKED 24

typedef struct {
    char     device[32];
    int      valid;
    uint64_t read_bytes;
    uint64_t write_bytes;
    int64_t  last_ms;
    double   read_peak;
    double   write_peak;
    int      hist_count;
    int      hist_head;
    double   read_hist[SLH_DISK_IO_HISTORY];
    double   write_hist[SLH_DISK_IO_HISTORY];
} disk_io_prev_t;

static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;
static disk_io_prev_t  g_io_prev[SLH_MAX_IO_TRACKED];
static size_t          g_io_count;

static disk_io_prev_t *io_lookup(const char *device)
{
    size_t i;

    for (i = 0; i < g_io_count; i++) {
        if (strcmp(g_io_prev[i].device, device) == 0)
            return &g_io_prev[i];
    }
    if (g_io_count >= SLH_MAX_IO_TRACKED)
        return NULL;
    memset(&g_io_prev[g_io_count], 0, sizeof(g_io_prev[0]));
    slh_strlcpy(g_io_prev[g_io_count].device, device,
                sizeof(g_io_prev[0].device));
    return &g_io_prev[g_io_count++];
}

/* ------------------------------------------------------------------ */
/* Auto-check bookkeeping (persisted)                                  */
/* ------------------------------------------------------------------ */

#define MAX_AUTOCHECKED 64

static pthread_mutex_t g_auto_lock = PTHREAD_MUTEX_INITIALIZER;
/* Devices whose automatic first check has *completed* and been persisted. */
static char            g_auto_done[MAX_AUTOCHECKED][32];
static size_t          g_auto_done_count;
/*
 * Devices whose automatic check is queued or running right now. Kept in
 * memory only: if the process dies mid-check we want the device to be
 * re-queued on the next start, which is exactly why the durable record is
 * written only after the check returns.
 */
static char            g_auto_pending[MAX_AUTOCHECKED][32];
static size_t          g_auto_pending_count;

static int name_in_list(char list[][32], size_t count, const char *device)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (strcmp(list[i], device) == 0)
            return 1;
    }
    return 0;
}

static void name_add(char list[][32], size_t *count, const char *device)
{
    if (*count >= MAX_AUTOCHECKED)
        return;
    if (name_in_list(list, *count, device))
        return;
    slh_strlcpy(list[*count], device, 32);
    (*count)++;
}

static int auto_already_done(const char *device)
{
    int found;

    pthread_mutex_lock(&g_auto_lock);
    found = name_in_list(g_auto_done, g_auto_done_count, device);
    pthread_mutex_unlock(&g_auto_lock);
    return found;
}

static int auto_is_pending(const char *device)
{
    int found;

    pthread_mutex_lock(&g_auto_lock);
    found = name_in_list(g_auto_pending, g_auto_pending_count, device);
    pthread_mutex_unlock(&g_auto_lock);
    return found;
}

static void auto_mark_pending(const char *device)
{
    pthread_mutex_lock(&g_auto_lock);
    name_add(g_auto_pending, &g_auto_pending_count, device);
    pthread_mutex_unlock(&g_auto_lock);
}

/**
 * Persist that `device` has had its automatic check.
 *
 * Called only once the check has actually finished: writing this up front
 * would leave a device permanently unchecked if the daemon restarted while
 * the check was in flight.
 */
static void auto_mark_done(const char *device)
{
    char  path[640];
    FILE *fp;

    pthread_mutex_lock(&g_auto_lock);
    name_add(g_auto_done, &g_auto_done_count, device);
    state_file_path(AUTOCHECK_FILE, path, sizeof(path));
    fp = path[0] ? fopen(path, "a") : NULL;
    if (fp) {
        fprintf(fp, "%s\n", device);
        fclose(fp);
    }
    pthread_mutex_unlock(&g_auto_lock);
}

static void auto_load_state(void)
{
    char  path[640];
    FILE *fp;
    char  line[128];

    state_file_path(AUTOCHECK_FILE, path, sizeof(path));
    if (!path[0])
        return;
    fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG("no auto-check record yet at %s", path);
        return;
    }

    pthread_mutex_lock(&g_auto_lock);
    while (fgets(line, sizeof(line), fp)) {
        char *name = slh_trim(line);

        if (*name == '\0' || *name == '#')
            continue;
        name_add(g_auto_done, &g_auto_done_count, name);
    }
    pthread_mutex_unlock(&g_auto_lock);
    fclose(fp);
    LOG_DEBUG("auto-check record %s lists %zu device(s)", path,
              g_auto_done_count);
}

/* ------------------------------------------------------------------ */
/* Check queue + worker thread                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char device[32];
    int  is_auto;          /* 1 when this is the first-sight automatic check */
} check_job_t;

static pthread_t       g_worker;
static int             g_worker_running;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond = PTHREAD_COND_INITIALIZER;
static check_job_t     g_queue[CHECK_QUEUE_LEN];
static int             g_queue_head;
static int             g_queue_count;
static int             g_queue_stop;
static char            g_in_progress[32];

/** Run all health sources for one device; returns the worst level seen. */
static disk_health_t check_device_health(const char *device, health_record_t *rec);

static void *worker_main(void *arg)
{
    SLH_UNUSED(arg);

    for (;;) {
        check_job_t     job;
        health_record_t result;

        pthread_mutex_lock(&g_queue_lock);
        while (g_queue_count == 0 && !g_queue_stop)
            pthread_cond_wait(&g_queue_cond, &g_queue_lock);
        if (g_queue_stop && g_queue_count == 0) {
            pthread_mutex_unlock(&g_queue_lock);
            break;
        }
        job = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % CHECK_QUEUE_LEN;
        g_queue_count--;
        slh_strlcpy(g_in_progress, job.device, sizeof(g_in_progress));
        pthread_mutex_unlock(&g_queue_lock);

        memset(&result, 0, sizeof(result));
        result.temperature_c = -1.0;
        result.auto_checked = job.is_auto;
        slh_strlcpy(result.device, job.device, sizeof(result.device));
        result.health = check_device_health(job.device, &result);
        result.valid = 1;
        slh_rfc3339_local((time_t)slh_now_sec(), result.checked_at,
                          sizeof(result.checked_at));

        pthread_mutex_lock(&g_health_lock);
        {
            health_record_t *slot = health_lookup(job.device, 1);

            if (slot)
                *slot = result;
            health_save_locked();       /* survive a restart */
        }
        pthread_mutex_unlock(&g_health_lock);

        pthread_mutex_lock(&g_queue_lock);
        g_in_progress[0] = '\0';
        pthread_mutex_unlock(&g_queue_lock);

        /* Persist the automatic check only now that it has really finished. */
        if (job.is_auto)
            auto_mark_done(job.device);

        LOG_INFO("disk %s health: %s (%s) - %s [%s]", job.device,
                 disk_health_name(result.health), result.source,
                 result.summary, job.is_auto ? "首次自动" : "手动");
    }
    return NULL;
}

void disk_init(const char *state_dir)
{
    g_state_dir[0] = '\0';
    if (state_dir && state_dir[0])
        slh_strlcpy(g_state_dir, state_dir, sizeof(g_state_dir));

    auto_load_state();
    health_load();

    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) {
        LOG_WARN("could not start the disk health worker");
        return;
    }
    g_worker_running = 1;
}

void disk_shutdown(void)
{
    if (!g_worker_running)
        return;
    pthread_mutex_lock(&g_queue_lock);
    g_queue_stop = 1;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_lock);
    pthread_join(g_worker, NULL);
    g_worker_running = 0;
}

static int queue_push(const char *device, int is_auto)
{
    int rc = -1;

    pthread_mutex_lock(&g_queue_lock);
    if (g_queue_count < CHECK_QUEUE_LEN) {
        int tail = (g_queue_head + g_queue_count) % CHECK_QUEUE_LEN;

        slh_strlcpy(g_queue[tail].device, device, sizeof(g_queue[0].device));
        g_queue[tail].is_auto = is_auto;
        g_queue_count++;
        pthread_cond_signal(&g_queue_cond);
        rc = 0;
    }
    pthread_mutex_unlock(&g_queue_lock);
    return rc;
}

int disk_request_check(const char *device)
{
    if (!device || !device[0] || !g_worker_running)
        return -1;
    return queue_push(device, 0);
}

int disk_device_exists(const char *device)
{
    disk_list_t list;
    size_t      i;

    disk_collect(&list);
    for (i = 0; i < list.count; i++) {
        if (strcmp(list.disk[i].device, device) == 0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Enumeration helpers                                                 */
/* ------------------------------------------------------------------ */

static int is_ignored_device(const char *name)
{
    static const char *prefixes[] = { "loop", "ram", "zram", "dm-", "sr",
                                      "md", "fd", NULL };
    size_t plen;
    int    i;

    for (i = 0; prefixes[i]; i++) {
        plen = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], plen) == 0)
            return 1;
    }
    /* eMMC boot / RPMB hardware partitions are not user-visible storage. */
    if (strstr(name, "boot0") || strstr(name, "boot1") || strstr(name, "rpmb"))
        return 1;
    return 0;
}

static void read_trimmed(const char *path, char *out, size_t out_size)
{
    char text[256];

    out[0] = '\0';
    if (slh_read_text(path, text, sizeof(text)))
        slh_strlcpy(out, slh_trim(text), out_size);
}

static void detect_transport(const char *device, char *out, size_t out_size)
{
    char path[256];
    char target[512];
    ssize_t n;

    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", SYS_BLOCK, device);
    n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0)
        return;
    target[n] = '\0';

    if (strstr(target, ".mmc"))
        slh_strlcpy(out, "mmc", out_size);
    else if (strstr(target, "nvme"))
        slh_strlcpy(out, "nvme", out_size);
    else if (strstr(target, "usb"))
        slh_strlcpy(out, "usb", out_size);
    else if (strstr(target, ".sata") || strstr(target, "ata"))
        slh_strlcpy(out, "sata", out_size);
    else if (strstr(target, "virtio"))
        slh_strlcpy(out, "virtio", out_size);
    else
        slh_strlcpy(out, "block", out_size);
}

static void classify_kind(disk_info_t *d)
{
    if (strncmp(d->device, "mmcblk", 6) == 0) {
        if (d->removable)
            slh_strlcpy(d->kind, "SD 卡", sizeof(d->kind));
        else
            slh_strlcpy(d->kind, "eMMC", sizeof(d->kind));
    } else if (strncmp(d->device, "nvme", 4) == 0) {
        slh_strlcpy(d->kind, "NVMe SSD", sizeof(d->kind));
    } else if (d->removable) {
        slh_strlcpy(d->kind, "可移动磁盘", sizeof(d->kind));
    } else if (d->rotational) {
        slh_strlcpy(d->kind, "HDD", sizeof(d->kind));
    } else {
        slh_strlcpy(d->kind, "SSD", sizeof(d->kind));
    }
}

/*
 * Mount points that mean "this disk is part of the operating system".
 * Checked most-significant first so `system_mount` reports the most
 * meaningful one when a disk carries several.
 */
static const char *const g_system_mounts[] = {
    "/", "/usr", "/etc", "/var", "/boot", "/opt", "/home", "/root", NULL
};

static int system_mount_rank(const char *mount_point)
{
    int i;

    for (i = 0; g_system_mounts[i]; i++) {
        if (strcmp(mount_point, g_system_mounts[i]) == 0)
            return i;
    }
    return -1;
}

/**
 * Find the mounted filesystem belonging to `device` (or one of its
 * partitions) and record the largest one.
 */
static void read_filesystem(disk_info_t *d)
{
    FILE *fp;
    char  line[512];
    size_t dev_len = strlen(d->device);

    fp = fopen(PROC_MOUNTS, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char *save = NULL;
        char *dev = strtok_r(line, " \t", &save);
        char *mnt = strtok_r(NULL, " \t", &save);
        char *fstype = strtok_r(NULL, " \t", &save);
        char *opts = strtok_r(NULL, " \t", &save);
        const char *base;
        struct statvfs vfs;
        uint64_t total;
        uint64_t freeb;

        if (!dev || !mnt || !fstype || !opts)
            continue;
        base = strrchr(dev, '/');
        base = base ? base + 1 : dev;

        /* Accept the whole device or one of its partitions. */
        if (strncmp(base, d->device, dev_len) != 0)
            continue;
        if (base[dev_len] != '\0' && base[dev_len] != 'p' &&
            !isdigit((unsigned char)base[dev_len]))
            continue;

        if (statvfs(mnt, &vfs) != 0)
            continue;
        total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;

        /* The 系统盘 badge tracks the most significant system mount on this
         * disk, independent of which filesystem wins the "largest" contest. */
        {
            int rank = system_mount_rank(mnt);

            if (rank >= 0 && (!d->is_system ||
                              rank < system_mount_rank(d->system_mount))) {
                d->is_system = 1;
                slh_strlcpy(d->system_mount, mnt, sizeof(d->system_mount));
            }
        }

        if (d->mounted && total <= d->fs_total_bytes)
            continue;                    /* keep the largest filesystem */

        d->mounted = 1;
        slh_strlcpy(d->fs_device, dev, sizeof(d->fs_device));
        slh_strlcpy(d->mount_point, mnt, sizeof(d->mount_point));
        slh_strlcpy(d->fs_type, fstype, sizeof(d->fs_type));
        d->fs_total_bytes = total;
        d->fs_free_bytes = freeb;
        d->fs_used_bytes = total > freeb ? total - freeb : 0;
        d->fs_used_percent = total ? (double)d->fs_used_bytes * 100.0 / (double)total
                                   : 0.0;

        /* "ro" as a standalone option means the volume was remounted
         * read-only, which is a classic post-error state. */
        d->fs_read_only = 0;
        {
            char *o = opts;
            while (*o) {
                char *comma = strchr(o, ',');
                size_t len = comma ? (size_t)(comma - o) : strlen(o);

                if (len == 2 && strncmp(o, "ro", 2) == 0) {
                    d->fs_read_only = 1;
                    break;
                }
                if (!comma)
                    break;
                o = comma + 1;
            }
        }
        LOG_DEBUG("disk %s: mount %s on %s (%s) opts='%s' ro=%d",
                  d->device, d->fs_device, d->mount_point, d->fs_type, opts,
                  d->fs_read_only);
    }
    fclose(fp);
}

/**
 * Read the cumulative I/O counters and, when `sample_io` is set, turn them
 * into bytes-per-second rates using the previous sample.
 */
static void read_io_counters(disk_info_t *d, int sample_io)
{
    char   path[256];
    FILE  *fp;
    unsigned long long f[17];
    int    n;

    snprintf(path, sizeof(path), "%s/%s/stat", SYS_BLOCK, d->device);
    fp = fopen(path, "r");
    if (!fp)
        return;
    memset(f, 0, sizeof(f));
    n = fscanf(fp, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                   "%llu %llu %llu %llu %llu %llu",
               &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7],
               &f[8], &f[9], &f[10], &f[11], &f[12], &f[13], &f[14], &f[15],
               &f[16]);
    fclose(fp);
    if (n < 7)
        return;

    d->read_bytes = (uint64_t)f[2] * 512u;    /* sectors read */
    d->write_bytes = (uint64_t)f[6] * 512u;   /* sectors written */
    d->read_bps = 0.0;
    d->write_bps = 0.0;

    if (!sample_io)
        return;

    {
        int64_t now = slh_monotonic_ms();

        pthread_mutex_lock(&g_io_lock);
        {
            disk_io_prev_t *prev = io_lookup(d->device);

            if (prev) {
                if (prev->valid && now > prev->last_ms) {
                    double dt = (double)(now - prev->last_ms) / 1000.0;

                    if (dt > 0.0) {
                        d->read_bps = d->read_bytes >= prev->read_bytes
                            ? (double)(d->read_bytes - prev->read_bytes) / dt
                            : 0.0;
                        d->write_bps = d->write_bytes >= prev->write_bytes
                            ? (double)(d->write_bytes - prev->write_bytes) / dt
                            : 0.0;
                    }
                }
                if (d->read_bps > prev->read_peak)
                    prev->read_peak = d->read_bps;
                if (d->write_bps > prev->write_peak)
                    prev->write_peak = d->write_bps;
                d->read_peak_bps = prev->read_peak;
                d->write_peak_bps = prev->write_peak;

                /* Push this sample onto the rolling window. */
                if (prev->valid) {
                    prev->read_hist[prev->hist_head] = d->read_bps;
                    prev->write_hist[prev->hist_head] = d->write_bps;
                    prev->hist_head = (prev->hist_head + 1) % SLH_DISK_IO_HISTORY;
                    if (prev->hist_count < SLH_DISK_IO_HISTORY)
                        prev->hist_count++;
                }
                d->io_hist_count = prev->hist_count;
                d->io_hist_head = prev->hist_head;
                memcpy(d->read_hist, prev->read_hist, sizeof(d->read_hist));
                memcpy(d->write_hist, prev->write_hist, sizeof(d->write_hist));

                /* Mean over the window: the number that actually moves on a
                 * mostly-idle disk, where the instantaneous rate is 0. */
                if (prev->hist_count > 0) {
                    double sum_r = 0.0;
                    double sum_w = 0.0;
                    int    i;

                    for (i = 0; i < prev->hist_count; i++) {
                        sum_r += prev->read_hist[i];
                        sum_w += prev->write_hist[i];
                    }
                    d->read_avg_bps = sum_r / (double)prev->hist_count;
                    d->write_avg_bps = sum_w / (double)prev->hist_count;
                }

                prev->valid = 1;
                prev->read_bytes = d->read_bytes;
                prev->write_bytes = d->write_bytes;
                prev->last_ms = now;
            }
        }
        pthread_mutex_unlock(&g_io_lock);
    }
}

/**
 * Read the filesystem's own error counter.
 *
 * ext4 publishes this at /sys/fs/ext4/<device>/errors_count, and unlike the
 * mount flags it is not affected by the service's read-only mount namespace.
 */
static void read_fs_errors(disk_info_t *d)
{
    char        path[320];
    const char *base;

    d->fs_errors_count = 0;
    if (!d->mounted || strcmp(d->fs_type, "ext4") != 0)
        return;

    base = strrchr(d->fs_device, '/');
    base = base ? base + 1 : d->fs_device;
    snprintf(path, sizeof(path), "/sys/fs/ext4/%.48s/errors_count", base);
    d->fs_errors_count = slh_read_u64(path, 0);
}

/* ------------------------------------------------------------------ */
/* Health: eMMC wear counters                                          */
/* ------------------------------------------------------------------ */

/**
 * Read the MMC controller's error counters from debugfs.
 *
 * A rising CRC/timeout count is a genuine degradation signal for a soldered
 * eMMC (or for its solder joints), and it is independent of the vendor's
 * wear buckets.
 *
 * Returns the sum of all counters, or -1 when unavailable - which is the
 * normal case under the shipped systemd unit, because
 * ProtectKernelTunables= masks /sys/kernel/debug from the service.
 */
static int64_t emmc_controller_errors(const char *device)
{
    char    link[512];
    char    resolved[PATH_MAX];
    char    path[PATH_MAX + 64];
    char   *host;
    char   *slash;
    FILE   *fp;
    char    line[128];
    int64_t total = 0;
    int     seen = 0;

    /*
     * The sysfs link is relative ("../../../mmc1:0001"), so it has to be
     * resolved before the host directory can be picked out of it.
     */
    snprintf(link, sizeof(link), "%s/%s/device", SYS_BLOCK, device);
    if (!realpath(link, resolved))
        return -1;

    /* .../mmc_host/mmc1/mmc1:0001 -> the debugfs directory is "mmc1". */
    host = strstr(resolved, "mmc_host/");
    if (!host)
        return -1;
    host += strlen("mmc_host/");
    slash = strchr(host, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    snprintf(path, sizeof(path), "/sys/kernel/debug/%s/err_stats", host);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *colon = strrchr(line, ':');

        if (!colon)
            continue;
        {
            char *end = NULL;
            long long v = strtoll(colon + 1, &end, 10);

            if (end != colon + 1) {
                total += v;
                seen = 1;
            }
        }
    }
    fclose(fp);
    return seen ? total : -1;
}

/**
 * MMC/eMMC controller error counters.
 *
 * This is deliberately *not* a wear or life-time metric: it only surfaces
 * CRC and timeout errors reported by the MMC host controller, which point at
 * a failing link or a bad solder joint. Returns UNKNOWN when the counters are
 * unreadable (the normal case for the shipped systemd unit) or when there is
 * nothing to report, so the filesystem check stays the verdict for such
 * devices instead of this one producing a meaningless "healthy".
 */
static disk_health_t check_mmc_errors(const char *device, health_record_t *rec)
{
    int64_t errors = emmc_controller_errors(device);
    char    line[192];

    if (errors <= 0)
        return DISK_HEALTH_UNKNOWN;

    slh_strlcpy(rec->source, "MMC 控制器", sizeof(rec->source));
    snprintf(rec->summary, sizeof(rec->summary),
             "MMC 控制器报告 %lld 次错误", (long long)errors);
    snprintf(line, sizeof(line),
             "控制器错误计数：%lld（CRC / 超时类错误，需留意排线或焊接）",
             (long long)errors);
    slh_strlcat(rec->detail, line, sizeof(rec->detail));
    return DISK_HEALTH_WARNING;
}

/* ------------------------------------------------------------------ */
/* Health: smartctl                                                    */
/* ------------------------------------------------------------------ */

/*
 * smartctl lives in different places depending on the distribution:
 * /usr/sbin on Debian and RHEL, /usr/bin on Arch and Alpine, /sbin on older
 * systems, and /usr/local/sbin when built from source. All of them are
 * tried, and $PATH is searched last so a relocated copy is still found.
 */
static const char *const g_smartctl_paths[] = {
    "/usr/sbin/smartctl",
    "/sbin/smartctl",
    "/usr/bin/smartctl",
    "/usr/local/sbin/smartctl",
    "/usr/local/bin/smartctl",
    "/opt/sbin/smartctl",
    NULL
};

/* Returns a pointer into a static buffer; valid until the next call. */
static const char *smartctl_path(void)
{
    static char resolved[PATH_MAX];
    const char *path_env;
    size_t      i;

    for (i = 0; g_smartctl_paths[i]; i++) {
        if (slh_is_executable(g_smartctl_paths[i]))
            return g_smartctl_paths[i];
    }

    path_env = getenv("PATH");
    while (path_env && *path_env) {
        const char *end = strchr(path_env, ':');
        size_t      len = end ? (size_t)(end - path_env) : strlen(path_env);

        if (len > 0 && len < sizeof(resolved) - 32) {
            int n = snprintf(resolved, sizeof(resolved), "%.*s/smartctl",
                             (int)len, path_env);

            if (n > 0 && (size_t)n < sizeof(resolved) &&
                slh_is_executable(resolved))
                return resolved;
        }
        if (!end)
            break;
        path_env = end + 1;
    }
    return NULL;
}

/**
 * Pull the RAW_VALUE column out of a `smartctl -A` row.
 *
 * The column layout is fixed up to WHEN_FAILED, whose field is "-" when the
 * attribute has never failed; the raw value starts right after the LAST such
 * dash. Falling back to the final token covers rows printed without it.
 */
static int smart_raw_value(const char *line, int64_t *out)
{
    char  copy[512];
    char *tokens[24];
    int   count = 0;
    int   last_dash = -1;
    int   i;
    char *save = NULL;
    char *tok;

    slh_strlcpy(copy, line, sizeof(copy));
    for (tok = strtok_r(copy, " \t", &save); tok && count < 24;
         tok = strtok_r(NULL, " \t", &save)) {
        tokens[count] = tok;
        if (strcmp(tok, "-") == 0)
            last_dash = count;
        count++;
    }
    if (count == 0)
        return 0;

    i = (last_dash >= 0 && last_dash + 1 < count) ? last_dash + 1 : count - 1;
    {
        char *end = NULL;
        long long v = strtoll(tokens[i], &end, 10);

        if (end == tokens[i])
            return 0;
        *out = (int64_t)v;
    }
    return 1;
}

/** Does the attribute row belong to `name`? */
static int smart_row_is(const char *line, const char *name)
{
    return strstr(line, name) != NULL;
}

static disk_health_t check_smart(const char *device, health_record_t *rec,
                                 disk_info_t *info)
{
    /*
     * Many SBC SATA ports sit behind a SAT layer, where smartctl cannot pick
     * the right protocol on its own and prints "Probable ATA device behind a
     * SAT layer" - while still exiting 0. Retry with an explicit device type
     * and stop as soon as a real health line comes back.
     */
    static const char *const devtypes[] = { NULL, "sat", "ata" };
    const size_t  devtype_count = sizeof(devtypes) / sizeof(devtypes[0]);
    char          dev_path[64];
    char          output[16384];
    const char   *tool = smartctl_path();
    char          used_devtype[16];
    char         *line;
    char         *save = NULL;
    int           rc = -1;
    disk_health_t level = DISK_HEALTH_UNKNOWN;
    int           saw_health = 0;
    size_t        attempt;

    if (!tool)
        return DISK_HEALTH_UNKNOWN;

    snprintf(dev_path, sizeof(dev_path), "/dev/%s", device);
    used_devtype[0] = '\0';

    for (attempt = 0; attempt < devtype_count; attempt++) {
        const char *argv[8];
        int         argc = 0;

        argv[argc++] = tool;
        argv[argc++] = "-H";
        argv[argc++] = "-i";
        argv[argc++] = "-A";
        if (devtypes[attempt]) {
            argv[argc++] = "-d";
            argv[argc++] = devtypes[attempt];
        }
        argv[argc++] = dev_path;
        argv[argc] = NULL;

        rc = slh_run_command(argv, output, sizeof(output), SMARTCTL_TIMEOUT_MS);
        if (rc >= 0) {
            if (devtypes[attempt])
                slh_strlcpy(used_devtype, devtypes[attempt],
                            sizeof(used_devtype));
            else
                used_devtype[0] = '\0';

            if (strstr(output, "self-assessment test result") ||
                strstr(output, "SMART Health Status"))
                break;                       /* got a real answer */
        }
        if (attempt + 1 >= devtype_count) {
            snprintf(used_devtype, sizeof(used_devtype), "unknown");
            break;
        }
        LOG_DEBUG("smartctl on %s gave no health line, retrying with -d %s",
                  device, devtypes[attempt + 1]);
    }

    if (rc < 0) {
        slh_strlcpy(rec->source, "smartctl", sizeof(rec->source));
        slh_strlcpy(rec->summary, "smartctl 执行失败或超时", sizeof(rec->summary));
        slh_strlcat(rec->detail, "无法读取 SMART 数据（执行失败或超时）。",
                    sizeof(rec->detail));
        /* Unknown, not warning: we learned nothing about the device. */
        return DISK_HEALTH_UNKNOWN;
    }

    /* A permission failure is an environment problem, not a disk problem.
     * Say so, and do not pretend the drive is degraded. */
    if (strstr(output, "Operation not permitted") ||
        strstr(output, "Permission denied")) {
        slh_strlcpy(rec->source, "smartctl", sizeof(rec->source));
        slh_strlcpy(rec->summary, "无权读取 SMART（缺少 CAP_SYS_RAWIO）",
                    sizeof(rec->summary));
        slh_strlcat(rec->detail,
                    "smartctl 无法直通 ATA 命令：服务需要 CAP_SYS_RAWIO 能力"
                    "与 disk 组成员身份（见 deploy/slhbond-monitor.service）。",
                    sizeof(rec->detail));
        return DISK_HEALTH_UNKNOWN;
    }

    slh_strlcpy(rec->source, "smartctl", sizeof(rec->source));

    for (line = strtok_r(output, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strstr(line, "overall-health self-assessment test result")) {
            saw_health = 1;
            if (strstr(line, "PASSED")) {
                level = DISK_HEALTH_HEALTHY;
                slh_strlcpy(rec->summary, "SMART 自检通过（PASSED）",
                            sizeof(rec->summary));
            } else if (strstr(line, "FAILED")) {
                level = DISK_HEALTH_FAULT;
                slh_strlcpy(rec->summary, "SMART 自检失败（FAILED）",
                            sizeof(rec->summary));
            }
            continue;
        }
        if (strstr(line, "SMART Health Status")) {
            saw_health = 1;
            if (strstr(line, "OK")) {
                level = DISK_HEALTH_HEALTHY;
                slh_strlcpy(rec->summary, "SMART 健康状态正常", sizeof(rec->summary));
            } else {
                level = DISK_HEALTH_WARNING;
                slh_strlcpy(rec->summary, "SMART 健康状态异常", sizeof(rec->summary));
            }
            continue;
        }

        /* Identity fields that smartctl reports more reliably than sysfs. */
        if (strstr(line, "Device Model:")) {
            char *v = strchr(line, ':');
            if (v)
                slh_strlcpy(rec->model, slh_trim(v + 1), sizeof(rec->model));
            continue;
        }
        if (strstr(line, "Serial Number:")) {
            char *v = strchr(line, ':');
            if (v)
                slh_strlcpy(rec->serial, slh_trim(v + 1), sizeof(rec->serial));
            continue;
        }
        if (strstr(line, "Firmware Version:")) {
            char *v = strchr(line, ':');
            if (v)
                slh_strlcpy(rec->firmware, slh_trim(v + 1),
                            sizeof(rec->firmware));
            continue;
        }
        if (strstr(line, "Rotation Rate:") && strstr(line, "Solid State"))
            info->rotational = 0;

        /* Attributes that indicate degradation. */
        {
            static const struct {
                const char *attr;
                int64_t     warn;
                int64_t     fault;
                const char *label;
            } attrs[] = {
                { "Reallocated_Sector_Ct",   1,  100, "重映射扇区数" },
                { "Current_Pending_Sector",  1,   16, "待映射扇区数" },
                { "Offline_Uncorrectable",   1,   16, "离线不可纠正扇区" },
                { "Reported_Uncorrect",      1,   16, "报告的不可纠正错误" },
                { NULL, 0, 0, NULL }
            };
            int i;

            for (i = 0; attrs[i].attr; i++) {
                int64_t value;

                if (!smart_row_is(line, attrs[i].attr))
                    continue;
                if (!smart_raw_value(line, &value))
                    continue;
                if (value >= attrs[i].fault) {
                    level = DISK_HEALTH_FAULT;
                    slh_strlcat(rec->detail, attrs[i].label, sizeof(rec->detail));
                    slh_strlcat(rec->detail, " 严重偏高；", sizeof(rec->detail));
                } else if (value >= attrs[i].warn) {
                    if (level < DISK_HEALTH_WARNING)
                        level = DISK_HEALTH_WARNING;
                    slh_strlcat(rec->detail, attrs[i].label, sizeof(rec->detail));
                    slh_strlcat(rec->detail, " 非零；", sizeof(rec->detail));
                }
                break;
            }
        }

        if (smart_row_is(line, "Temperature_Celsius") ||
            smart_row_is(line, "Airflow_Temperature_Cel")) {
            int64_t value;

            if (smart_raw_value(line, &value) && value > 0 && value < 200) {
                rec->temperature_c = (double)value;
                if (value >= 70) {
                    level = DISK_HEALTH_FAULT;
                    slh_strlcat(rec->detail, "温度过高；", sizeof(rec->detail));
                } else if (value >= 60) {
                    if (level < DISK_HEALTH_WARNING)
                        level = DISK_HEALTH_WARNING;
                    slh_strlcat(rec->detail, "温度偏高；", sizeof(rec->detail));
                }
            }
        }

        if (smart_row_is(line, "Power_On_Hours")) {
            int64_t value;

            if (smart_raw_value(line, &value) && value >= 0)
                rec->power_on_hours = (uint64_t)value;
        }
    }

    if (!saw_health && level == DISK_HEALTH_UNKNOWN) {
        slh_strlcpy(rec->summary, "设备未提供 SMART 健康结论",
                    sizeof(rec->summary));
        /* Still unknown: the drive simply does not expose a verdict. */
    } else if (level == DISK_HEALTH_HEALTHY) {
        if (rec->summary[0] == '\0')
            slh_strlcpy(rec->summary, "SMART 状态正常", sizeof(rec->summary));
    }

    if (rec->detail[0])
        slh_strlcat(rec->detail, "\n", sizeof(rec->detail));
    slh_strlcat(rec->detail, "来源：smartctl -H -i -A ", sizeof(rec->detail));
    if (used_devtype[0] && strcmp(used_devtype, "unknown") != 0) {
        slh_strlcat(rec->detail, "-d ", sizeof(rec->detail));
        slh_strlcat(rec->detail, used_devtype, sizeof(rec->detail));
        slh_strlcat(rec->detail, " ", sizeof(rec->detail));
    }
    slh_strlcat(rec->detail, dev_path, sizeof(rec->detail));
    return level;
}

/* ------------------------------------------------------------------ */
/* Health: filesystem fallback                                         */
/* ------------------------------------------------------------------ */

static disk_health_t check_filesystem(const disk_info_t *info,
                                      health_record_t *rec)
{
    disk_health_t level = DISK_HEALTH_UNKNOWN;
    char          line[192];

    if (!info->mounted)
        return DISK_HEALTH_UNKNOWN;

    /* Keep each source on its own line in the detail blob. */
    if (rec->detail[0])
        slh_strlcat(rec->detail, "\n", sizeof(rec->detail));

    /*
     * Deliberately NOT using the read-only mount flag here: ProtectSystem
     * remounts the service's whole namespace read-only, so that flag is
     * meaningless from in here. The filesystem's own error counter is the
     * trustworthy signal, and it is namespace-independent.
     */
    if (info->fs_errors_count > 0) {
        snprintf(line, sizeof(line), "文件系统错误计数 %llu；",
                 (unsigned long long)info->fs_errors_count);
        slh_strlcat(rec->detail, line, sizeof(rec->detail));
        level = info->fs_errors_count > 10 ? DISK_HEALTH_FAULT
                                           : DISK_HEALTH_WARNING;
    }

    if (info->fs_total_bytes > 0) {
        double free_pct = 100.0 - info->fs_used_percent;

        snprintf(line, sizeof(line), "剩余空间 %.1f%%；", free_pct);
        slh_strlcat(rec->detail, line, sizeof(rec->detail));

        if (free_pct < 2.0) {
            level = DISK_HEALTH_FAULT;
            slh_strlcat(rec->detail, "可用空间严重不足；", sizeof(rec->detail));
        } else if (free_pct < 5.0) {
            if (level < DISK_HEALTH_WARNING)
                level = DISK_HEALTH_WARNING;
            slh_strlcat(rec->detail, "可用空间偏低；", sizeof(rec->detail));
        } else if (level == DISK_HEALTH_UNKNOWN) {
            level = DISK_HEALTH_HEALTHY;
        }
    }

    if (rec->source[0] == '\0')
        slh_strlcpy(rec->source, "文件系统", sizeof(rec->source));
    if (rec->summary[0] == '\0') {
        if (level == DISK_HEALTH_HEALTHY)
            slh_strlcpy(rec->summary, "文件系统可用空间充足", sizeof(rec->summary));
        else if (level == DISK_HEALTH_WARNING)
            slh_strlcpy(rec->summary, "文件系统需要注意", sizeof(rec->summary));
        else
            slh_strlcpy(rec->summary, "文件系统可用空间不足", sizeof(rec->summary));
    }
    return level;
}

/* ------------------------------------------------------------------ */
/* Combined check                                                      */
/* ------------------------------------------------------------------ */

static disk_health_t check_device_health(const char *device, health_record_t *rec)
{
    disk_list_t   list;
    disk_info_t  *info = NULL;
    disk_health_t worst = DISK_HEALTH_UNKNOWN;
    disk_health_t level;
    size_t        i;

    /* Re-enumerate so the check sees current mounts and counters. Rates are
     * deliberately not sampled here: this runs between polls and would
     * shorten the sampling window for the dashboard. */
    disk_collect_internal(&list, 0);
    for (i = 0; i < list.count; i++) {
        if (strcmp(list.disk[i].device, device) == 0) {
            info = &list.disk[i];
            break;
        }
    }
    if (!info)
        return DISK_HEALTH_UNKNOWN;

    /* smartctl may enrich identity fields, so let it run first. */
    if (strncmp(device, "sd", 2) == 0 || strncmp(device, "nvme", 4) == 0 ||
        strncmp(device, "hd", 2) == 0) {
        level = check_smart(device, rec, info);
        if (level > worst)
            worst = level;
    }

    level = check_mmc_errors(device, rec);
    if (level > worst)
        worst = level;

    level = check_filesystem(info, rec);
    if (level > worst)
        worst = level;

    if (worst == DISK_HEALTH_UNKNOWN)
        slh_strlcpy(rec->summary, "该设备未提供可用的健康数据",
                    sizeof(rec->summary));
    return worst;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void disk_collect(disk_list_t *out)
{
    disk_collect_internal(out, 1);
}

static void disk_collect_internal(disk_list_t *out, int sample_io)
{
    DIR           *dir;
    struct dirent *entry;

    memset(out, 0, sizeof(*out));

    dir = opendir(SYS_BLOCK);
    if (!dir) {
        LOG_WARN("cannot open %s", SYS_BLOCK);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        disk_info_t *d;
        char         path[320];
        uint64_t     sectors;

        if (entry->d_name[0] == '.')
            continue;
        if (is_ignored_device(entry->d_name))
            continue;
        if (out->count >= SLH_MAX_DISKS)
            break;

        d = &out->disk[out->count];
        memset(d, 0, sizeof(*d));
        d->temperature_c = -1.0;
        slh_strlcpy(d->device, entry->d_name, sizeof(d->device));

        snprintf(path, sizeof(path), "%s/%s/size", SYS_BLOCK, entry->d_name);
        sectors = slh_read_u64(path, 0);
        if (sectors == 0)
            continue;                    /* not a usable block device */
        d->size_bytes = sectors * 512u;

        snprintf(path, sizeof(path), "%s/%s/removable", SYS_BLOCK, entry->d_name);
        d->removable = slh_read_u64(path, 0) ? 1 : 0;
        snprintf(path, sizeof(path), "%s/%s/queue/rotational", SYS_BLOCK,
                 entry->d_name);
        d->rotational = slh_read_u64(path, 0) ? 1 : 0;

        snprintf(path, sizeof(path), "%s/%s/device/model", SYS_BLOCK,
                 entry->d_name);
        read_trimmed(path, d->name, sizeof(d->name));
        if (d->name[0] == '\0') {
            /* eMMC and SD cards report a short product name instead. */
            snprintf(path, sizeof(path), "%s/%s/device/name", SYS_BLOCK,
                     entry->d_name);
            read_trimmed(path, d->name, sizeof(d->name));
        }
        if (d->name[0] == '\0')
            slh_strlcpy(d->name, d->device, sizeof(d->name));

        snprintf(path, sizeof(path), "%s/%s/device/vendor", SYS_BLOCK,
                 entry->d_name);
        read_trimmed(path, d->vendor, sizeof(d->vendor));
        snprintf(path, sizeof(path), "%s/%s/device/serial", SYS_BLOCK,
                 entry->d_name);
        read_trimmed(path, d->serial, sizeof(d->serial));
        if (d->serial[0] == '\0') {
            snprintf(path, sizeof(path), "%s/%s/device/cid", SYS_BLOCK,
                     entry->d_name);
            read_trimmed(path, d->serial, sizeof(d->serial));
        }
        snprintf(path, sizeof(path), "%s/%s/device/fwrev", SYS_BLOCK,
                 entry->d_name);
        read_trimmed(path, d->firmware, sizeof(d->firmware));
        if (d->firmware[0] == '\0') {
            snprintf(path, sizeof(path), "%s/%s/device/rev", SYS_BLOCK,
                     entry->d_name);
            read_trimmed(path, d->firmware, sizeof(d->firmware));
        }

        detect_transport(d->device, d->transport, sizeof(d->transport));
        classify_kind(d);
        read_filesystem(d);
        read_fs_errors(d);
        read_io_counters(d, sample_io);

        /* Merge whatever health result is already known. */
        pthread_mutex_lock(&g_health_lock);
        {
            health_record_t *slot = health_lookup(d->device, 0);

            if (slot && slot->valid) {
                d->health_checked = 1;
                d->health = slot->health;
                d->health_auto = slot->auto_checked;
                d->temperature_c = slot->temperature_c;
                d->power_on_hours = slot->power_on_hours;
                slh_strlcpy(d->health_source, slot->source,
                            sizeof(d->health_source));
                slh_strlcpy(d->health_summary, slot->summary,
                            sizeof(d->health_summary));
                slh_strlcpy(d->health_detail, slot->detail,
                            sizeof(d->health_detail));
                slh_strlcpy(d->health_checked_at, slot->checked_at,
                            sizeof(d->health_checked_at));

                /* smartctl knows the serial/firmware on drives whose sysfs
                 * attributes are empty (many USB-SATA bridges, for one). */
                if (d->serial[0] == '\0' && slot->serial[0])
                    slh_strlcpy(d->serial, slot->serial, sizeof(d->serial));
                if (d->firmware[0] == '\0' && slot->firmware[0])
                    slh_strlcpy(d->firmware, slot->firmware,
                                sizeof(d->firmware));
                if (slot->model[0] &&
                    (d->name[0] == '\0' || strcmp(d->name, d->device) == 0))
                    slh_strlcpy(d->name, slot->model, sizeof(d->name));
            }
        }
        pthread_mutex_unlock(&g_health_lock);

        out->count++;
    }
    closedir(dir);

    /* Surface the queue state so the UI can show a spinner. */
    pthread_mutex_lock(&g_queue_lock);
    slh_strlcpy(out->check_device, g_in_progress, sizeof(out->check_device));
    out->check_in_progress = g_in_progress[0] != '\0' || g_queue_count > 0;
    pthread_mutex_unlock(&g_queue_lock);

    /*
     * First-sight automatic check: a disk we have never checked gets exactly
     * one. The durable record is written by the worker *after* the check
     * completes, so a restart mid-check simply retries instead of leaving the
     * device permanently unassessed. After that, only disk_request_check()
     * (the UI button) triggers another.
     */
    {
        size_t i;

        for (i = 0; i < out->count; i++) {
            disk_info_t *d = &out->disk[i];

            if (d->health_checked || auto_already_done(d->device) ||
                auto_is_pending(d->device))
                continue;
            auto_mark_pending(d->device);
            if (queue_push(d->device, 1) == 0)
                LOG_INFO("queued first-time health check for %s", d->device);
        }
    }
}

const char *disk_health_name(disk_health_t level)
{
    switch (level) {
    case DISK_HEALTH_HEALTHY: return "healthy";
    case DISK_HEALTH_WARNING: return "warning";
    case DISK_HEALTH_FAULT:   return "fault";
    default:                  return "unknown";
    }
}

void disk_to_json(strbuf_t *sb, const disk_info_t *d, int *first)
{
    size_t i;

    json_add_str(sb, "device", d->device, first);
    json_add_str(sb, "name", d->name, first);
    json_add_str(sb, "kind", d->kind, first);
    json_add_str(sb, "transport", d->transport, first);
    json_add_str(sb, "vendor", d->vendor[0] ? d->vendor : NULL, first);
    json_add_str(sb, "serial", d->serial[0] ? d->serial : NULL, first);
    json_add_str(sb, "firmware", d->firmware[0] ? d->firmware : NULL, first);
    json_add_bool(sb, "removable", d->removable, first);
    json_add_bool(sb, "rotational", d->rotational, first);
    json_add_uint(sb, "size_bytes", d->size_bytes, first);

    json_add_bool(sb, "mounted", d->mounted, first);
    json_add_bool(sb, "is_system", d->is_system, first);
    json_add_str(sb, "system_mount",
                 d->system_mount[0] ? d->system_mount : NULL, first);
    json_add_str(sb, "mount_point", d->mounted ? d->mount_point : NULL, first);
    json_add_str(sb, "fs_type", d->mounted ? d->fs_type : NULL, first);
    json_add_str(sb, "fs_device", d->mounted ? d->fs_device : NULL, first);
    json_add_bool(sb, "fs_read_only", d->fs_read_only, first);
    json_add_uint(sb, "fs_total_bytes", d->fs_total_bytes, first);
    json_add_uint(sb, "fs_free_bytes", d->fs_free_bytes, first);
    json_add_uint(sb, "fs_used_bytes", d->fs_used_bytes, first);
    json_add_double(sb, "fs_used_percent", d->fs_used_percent, first);
    json_add_uint(sb, "fs_errors_count", d->fs_errors_count, first);

    json_add_uint(sb, "read_bytes", d->read_bytes, first);
    json_add_uint(sb, "write_bytes", d->write_bytes, first);
    json_add_double(sb, "read_bps", d->read_bps, first);
    json_add_double(sb, "write_bps", d->write_bps, first);
    json_add_double(sb, "read_peak_bps", d->read_peak_bps, first);
    json_add_double(sb, "write_peak_bps", d->write_peak_bps, first);
    json_add_double(sb, "read_avg_bps", d->read_avg_bps, first);
    json_add_double(sb, "write_avg_bps", d->write_avg_bps, first);
    json_add_int(sb, "io_history_length", SLH_DISK_IO_HISTORY, first);

    /* Rolling window, oldest first, so the sparkline reads left to right. */
    json_key(sb, "read_history", first);
    sb_appendc(sb, '[');
    for (i = 0; i < (size_t)d->io_hist_count; i++) {
        int idx = (d->io_hist_head - d->io_hist_count + (int)i
                   + SLH_DISK_IO_HISTORY) % SLH_DISK_IO_HISTORY;
        if (i)
            sb_appendc(sb, ',');
        sb_appendf(sb, "%.0f", d->read_hist[idx]);
    }
    sb_appendc(sb, ']');

    json_key(sb, "write_history", first);
    sb_appendc(sb, '[');
    for (i = 0; i < (size_t)d->io_hist_count; i++) {
        int idx = (d->io_hist_head - d->io_hist_count + (int)i
                   + SLH_DISK_IO_HISTORY) % SLH_DISK_IO_HISTORY;
        if (i)
            sb_appendc(sb, ',');
        sb_appendf(sb, "%.0f", d->write_hist[idx]);
    }
    sb_appendc(sb, ']');

    json_add_str(sb, "health", disk_health_name(d->health), first);
    json_add_bool(sb, "health_checked", d->health_checked, first);
    json_add_bool(sb, "health_auto", d->health_auto, first);
    json_add_str(sb, "health_source",
                 d->health_source[0] ? d->health_source : NULL, first);
    json_add_str(sb, "health_summary",
                 d->health_summary[0] ? d->health_summary : NULL, first);
    json_add_str(sb, "health_detail",
                 d->health_detail[0] ? d->health_detail : NULL, first);
    json_add_str(sb, "health_checked_at",
                 d->health_checked_at[0] ? d->health_checked_at : NULL, first);

    if (d->temperature_c < 0.0)
        json_add_null(sb, "temperature_c", first);
    else
        json_add_double(sb, "temperature_c", d->temperature_c, first);
    json_add_uint(sb, "power_on_hours", d->power_on_hours, first);
}
