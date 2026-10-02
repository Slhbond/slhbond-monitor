/*
 * sysinfo.h - host identity, uptime and lightweight resource metrics.
 */
#ifndef SLH_SYSINFO_H
#define SLH_SYSINFO_H

#include <stdint.h>

#include "util.h"

#define SLH_UPTIME_HUMAN_LEN 64

typedef struct {
    /* --- identity ------------------------------------------------- */
    char platform[64];      /* uname(2) sysname, e.g. "Linux"            */
    char arch[64];          /* uname(2) machine, e.g. "x86_64"           */
    char os_id[64];         /* /etc/os-release ID, e.g. "ubuntu"         */
    char os_version[160];   /* /etc/os-release PRETTY_NAME               */
    char kernel[160];       /* uname(2) release                          */
    char hostname[256];

    /* --- uptime --------------------------------------------------- */
    uint64_t uptime_seconds;
    int64_t  boot_time;                        /* epoch seconds */

    /* --- resource metrics (for /api/v1/metrics) ------------------- */
    /* CPU load and core counts live in cpustat.h - this module no longer
     * samples them, so there is exactly one source of CPU truth.        */
    double   load1, load5, load15;
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    uint64_t mem_used_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    uint64_t disk_total_kb;
    uint64_t disk_free_kb;
    char     disk_mount[64];
} sysinfo_t;

/**
 * Fill `si` from uname(2), /etc/os-release, /proc and statvfs(3).
 * Every field has a sane fallback so the caller never sees garbage.
 * Safe to call from multiple threads.
 */
void sysinfo_collect(sysinfo_t *si);

/** Emit the identity members (what the dashboard card renders). */
void sysinfo_to_json(strbuf_t *sb, const sysinfo_t *si, int *first);

/** Emit the resource metrics members. */
void sysinfo_metrics_to_json(strbuf_t *sb, const sysinfo_t *si, int *first);

#endif /* SLH_SYSINFO_H */
