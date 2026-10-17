/*
 * cpustat.h - detailed CPU view: per-core load, model, topology, frequency
 *             and temperature.
 *
 * This module owns *all* CPU sampling. sysinfo.c deliberately no longer
 * computes a CPU percentage, so the dashboard and /api/v1/metrics can never
 * disagree about how busy the CPU is.
 *
 * Thread safety: cpustat_collect() is serialised internally, and the
 * previous-sample cache it needs lives behind its own mutex.
 */
#ifndef SLH_CPUSTAT_H
#define SLH_CPUSTAT_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SLH_MAX_CPUS 128

typedef struct {
    int      index;           /* cpuN index                              */
    double   usage_percent;   /* -1.0 until the second sample arrives    */
    uint64_t freq_khz;        /* 0 when cpufreq is unavailable           */
} cpustat_core_t;

typedef struct {
    /* --- identity ------------------------------------------------- */
    char model[192];          /* /proc/cpuinfo "model name" (may be generic) */
    char soc[96];             /* "Rockchip RK3568" from the device tree      */
    char machine[96];         /* device-tree model, e.g. "dg-tn3568"         */
    char core_name[64];       /* decoded ARM part, e.g. "Cortex-A55"         */

    /* --- topology ------------------------------------------------- */
    int    cores;             /* physical cores (unique package:core_id)     */
    int    threads;           /* logical CPUs                                */

    /* --- load ----------------------------------------------------- */
    double usage_percent;     /* mean across cores, -1.0 when unknown        */
    double max_core_percent;  /* busiest single thread, -1.0 when unknown    */
    int    busiest_core;      /* its index, -1 when unknown                  */

    /* --- frequency ------------------------------------------------ */
    uint64_t freq_cur_khz;    /* fastest currently-running core              */
    uint64_t freq_min_khz;
    uint64_t freq_max_khz;
    char     governor[32];

    /* --- thermal -------------------------------------------------- */
    double temp_c;            /* hottest CPU-related zone, -1.0 when unknown */
    char   temp_label[64];    /* e.g. "cpu-thermal"                          */

    /* --- per-core detail ------------------------------------------ */
    size_t        core_count;
    cpustat_core_t core[SLH_MAX_CPUS];
} cpustat_t;

/** Sample /proc/stat, /proc/cpuinfo, the device tree, cpufreq and thermal. */
void cpustat_collect(cpustat_t *cs);

/** Emit the whole CPU view as JSON members. */
void cpustat_to_json(strbuf_t *sb, const cpustat_t *cs, int *first);

#endif /* SLH_CPUSTAT_H */
