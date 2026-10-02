/*
 * sysinfo.c - host identity, uptime and resource metrics.
 *
 * Everything is read from standard Linux interfaces (/proc, statvfs, uname),
 * so the daemon needs no dependencies and no elevated privileges.
 */
#include "sysinfo.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "log.h"

#define OS_RELEASE_PATH "/etc/os-release"
#define PROC_UPTIME     "/proc/uptime"
#define PROC_LOADAVG    "/proc/loadavg"
#define PROC_MEMINFO    "/proc/meminfo"

/* ------------------------------------------------------------------ */
/* /etc/os-release                                                     */
/* ------------------------------------------------------------------ */

/**
 * Strip surrounding quotes and process backslash escapes, in place.
 *
 * The shifting is done with memmove rather than by advancing the pointer:
 * `value` is a local copy, so a `value++` here would be invisible to the
 * caller and leave the opening quote behind.
 */
static void unquote(char *value)
{
    size_t len = strlen(value);
    char  *src;
    char  *dst;

    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
    for (src = dst = value; *src; src++) {
        if (*src == '\\' && src[1])
            src++;
        *dst++ = *src;
    }
    *dst = '\0';
}

static int os_release_lookup(const char *key, char *out, size_t out_size)
{
    FILE *fp;
    char  line[512];

    out[0] = '\0';
    fp = fopen(OS_RELEASE_PATH, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        char *value;

        value = slh_trim(line);
        if (*value == '\0' || *value == '#')
            continue;
        eq = strchr(value, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(slh_trim(value), key) != 0)
            continue;

        value = slh_trim(eq + 1);
        unquote(value);
        slh_strlcpy(out, value, out_size);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc readers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t read_uptime_seconds(void)
{
    FILE *fp;
    double seconds = 0.0;

    fp = fopen(PROC_UPTIME, "r");
    if (!fp)
        return 0;
    if (fscanf(fp, "%lf", &seconds) != 1)
        seconds = 0.0;
    fclose(fp);
    return seconds > 0.0 ? (uint64_t)seconds : 0;
}

static void read_loadavg(double *l1, double *l5, double *l15)
{
    FILE *fp;

    *l1 = *l5 = *l15 = 0.0;
    fp = fopen(PROC_LOADAVG, "r");
    if (!fp)
        return;
    if (fscanf(fp, "%lf %lf %lf", l1, l5, l15) != 3)
        *l1 = *l5 = *l15 = 0.0;
    fclose(fp);
}

/** Look up one "Key: <value> kB" entry in /proc/meminfo. */
static uint64_t meminfo_lookup(const char *key, uint64_t fallback)
{
    FILE *fp;
    char  line[256];
    size_t key_len = strlen(key);

    fp = fopen(PROC_MEMINFO, "r");
    if (!fp)
        return fallback;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != ':')
            continue;
        {
            unsigned long long value = 0;
            if (sscanf(line + key_len + 1, "%llu", &value) == 1) {
                fclose(fp);
                return (uint64_t)value;
            }
        }
    }
    fclose(fp);
    return fallback;
}

static void read_disk(const char *mount, uint64_t *total_kb, uint64_t *free_kb)
{
    struct statvfs vfs;

    *total_kb = 0;
    *free_kb = 0;
    if (statvfs(mount, &vfs) != 0)
        return;
    {
        uint64_t block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        *total_kb = (uint64_t)vfs.f_blocks * block / 1024u;
        *free_kb = (uint64_t)vfs.f_bavail * block / 1024u;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void sysinfo_collect(sysinfo_t *si)
{
    struct utsname uts;

    memset(si, 0, sizeof(*si));
    slh_strlcpy(si->disk_mount, "/", sizeof(si->disk_mount));

    if (uname(&uts) == 0) {
        slh_strlcpy(si->platform, uts.sysname, sizeof(si->platform));
        slh_strlcpy(si->arch, uts.machine, sizeof(si->arch));
        slh_strlcpy(si->kernel, uts.release, sizeof(si->kernel));
        slh_strlcpy(si->hostname, uts.nodename, sizeof(si->hostname));
    } else {
        LOG_WARN("uname(2) failed; falling back to generic identity");
        slh_strlcpy(si->platform, "Linux", sizeof(si->platform));
        slh_strlcpy(si->arch, "unknown", sizeof(si->arch));
    }

    if (gethostname(si->hostname, sizeof(si->hostname) - 1) != 0)
        slh_strlcpy(si->hostname, "unknown", sizeof(si->hostname));
    si->hostname[sizeof(si->hostname) - 1] = '\0';

    /* A nicer display name than the bare "Linux" when we can get one. */
    if (!os_release_lookup("PRETTY_NAME", si->os_version, sizeof(si->os_version))) {
        if (si->kernel[0])
            snprintf(si->os_version, sizeof(si->os_version), "Linux %.120s", si->kernel);
        else
            slh_strlcpy(si->os_version, "Linux", sizeof(si->os_version));
    }
    if (!os_release_lookup("ID", si->os_id, sizeof(si->os_id)))
        slh_strlcpy(si->os_id, "linux", sizeof(si->os_id));
    if (si->platform[0] == '\0')
        slh_strlcpy(si->platform, "Linux", sizeof(si->platform));

    si->uptime_seconds = read_uptime_seconds();
    si->boot_time = slh_now_sec() - (int64_t)si->uptime_seconds;

    read_loadavg(&si->load1, &si->load5, &si->load15);

    si->mem_total_kb = meminfo_lookup("MemTotal", 0);
    si->mem_available_kb = meminfo_lookup("MemAvailable", 0);
    if (si->mem_available_kb == 0)
        si->mem_available_kb = meminfo_lookup("MemFree", 0);
    si->mem_used_kb = si->mem_total_kb > si->mem_available_kb
                          ? si->mem_total_kb - si->mem_available_kb
                          : 0;
    si->swap_total_kb = meminfo_lookup("SwapTotal", 0);
    si->swap_free_kb = meminfo_lookup("SwapFree", 0);

    read_disk(si->disk_mount, &si->disk_total_kb, &si->disk_free_kb);
}

void sysinfo_to_json(strbuf_t *sb, const sysinfo_t *si, int *first)
{
    char uptime_human[SLH_UPTIME_HUMAN_LEN];
    char server_time[48];

    slh_format_uptime(si->uptime_seconds, uptime_human, sizeof(uptime_human));
    slh_rfc3339_local((time_t)slh_now_sec(), server_time, sizeof(server_time));

    json_add_str(sb, "platform", si->platform, first);
    json_add_str(sb, "arch", si->arch, first);
    json_add_str(sb, "os_id", si->os_id, first);
    json_add_str(sb, "os_version", si->os_version, first);
    json_add_str(sb, "kernel", si->kernel, first);
    json_add_str(sb, "hostname", si->hostname, first);
    json_add_uint(sb, "uptime_seconds", si->uptime_seconds, first);
    json_add_str(sb, "uptime_human", uptime_human, first);
    json_add_int(sb, "boot_time", si->boot_time, first);
    json_add_str(sb, "server_time", server_time, first);
    json_add_str(sb, "status", "online", first);
}

void sysinfo_metrics_to_json(strbuf_t *sb, const sysinfo_t *si, int *first)
{
    char buf[64];

    json_add_double(sb, "load1", si->load1, first);
    json_add_double(sb, "load5", si->load5, first);
    json_add_double(sb, "load15", si->load15, first);

    json_add_uint(sb, "mem_total_kb", si->mem_total_kb, first);
    json_add_uint(sb, "mem_used_kb", si->mem_used_kb, first);
    json_add_uint(sb, "mem_available_kb", si->mem_available_kb, first);
    slh_format_bytes(si->mem_total_kb * 1024u, buf, sizeof(buf));
    json_add_str(sb, "mem_total_human", buf, first);
    slh_format_bytes(si->mem_used_kb * 1024u, buf, sizeof(buf));
    json_add_str(sb, "mem_used_human", buf, first);

    json_add_uint(sb, "swap_total_kb", si->swap_total_kb, first);
    json_add_uint(sb, "swap_free_kb", si->swap_free_kb, first);

    json_add_uint(sb, "disk_total_kb", si->disk_total_kb, first);
    json_add_uint(sb, "disk_free_kb", si->disk_free_kb, first);
    json_add_str(sb, "disk_mount", si->disk_mount, first);
    slh_format_bytes(si->disk_total_kb * 1024u, buf, sizeof(buf));
    json_add_str(sb, "disk_total_human", buf, first);
    slh_format_bytes(si->disk_free_kb * 1024u, buf, sizeof(buf));
    json_add_str(sb, "disk_free_human", buf, first);
}
