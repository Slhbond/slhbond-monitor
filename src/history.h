/*
 * history.h - compact per-second hardware time series, kept on local disk.
 *
 * Design constraints, in order of importance:
 *
 *   1. **Little disk.** A full JSON snapshot per second would be tens of
 *      kilobytes; the binary record here is 32 bytes plus one byte per CPU
 *      core (36 bytes on a 4-core board). A whole day is ~3 MB.
 *   2. **Little flash wear.** Records are buffered in memory and written in
 *      batches, so the file is touched once per `history_flush_sec` instead
 *      of once per second.
 *   3. **Bounded.** The file is preallocated to a fixed capacity and used as
 *      a ring, so it never grows past `history_hours` worth of records.
 *
 * A power cut can lose at most `history_flush_sec` of the newest samples -
 * a deliberate trade against wearing out an eMMC with one fsync per second.
 *
 * Thread safety: append is called from the sampler thread only; queries come
 * from HTTP worker threads. Both go through one mutex.
 */
#ifndef SLH_HISTORY_H
#define SLH_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Stored per record; boards with more cores keep only the first 16. */
#define SLH_HIST_MAX_CORES 16

/* 0xFFFF / 255 mean "not available" in the packed record. */
#define SLH_HIST_NA_U16 0xFFFFu
#define SLH_HIST_NA_U8  0xFFu

/** One sample, in engineering units, as handed to history_append(). */
typedef struct {
    int64_t ts;                       /* epoch seconds                    */
    double  cpu_avg;                  /* mean across cores,  -1 = n/a     */
    double  cpu_max;                  /* busiest core,        -1 = n/a    */
    double  mem_pct;                  /* 0..100,              -1 = n/a    */
    double  load1;                    /* 1-minute load average            */
    double  temp_c;                   /* CPU temperature,     -1 = n/a    */
    double  freq_mhz;                 /* 0 = n/a                          */
    double  net_rx_bps;               /* primary interface,   -1 = n/a    */
    double  net_tx_bps;
    double  disk_r_bps;               /* summed over disks,   -1 = n/a    */
    double  disk_w_bps;
    double  gpu_pct;                  /* -1 = n/a                         */
    double  npu_pct;                  /* -1 = n/a                         */
    double  gpu_temp_c;               /* -1 = n/a                         */
    double  gpu_freq_mhz;             /* 0 = n/a                          */
    double  disk_temp_c;              /* hottest disk, -1 = n/a           */
    int     ncores;                   /* number of valid entries in core[] */
    double  core[SLH_HIST_MAX_CORES];
} hist_sample_t;

/** One decoded record, as returned by history_query(). */
typedef struct {
    int64_t ts;
    int     cpu_avg;      /* -1 = n/a, else 0..100 (saturating)          */
    int     cpu_max;
    int     mem_pct;
    int     temp_c10;     /* tenths of °C, -1 = n/a                      */
    int     freq_mhz;
    int     load1_x100;
    uint32_t net_rx_bps;
    uint32_t net_tx_bps;
    uint32_t disk_r_bps;
    uint32_t disk_w_bps;
    int     gpu_pct;
    int     npu_pct;
    int     gpu_temp_c10; /* tenths of °C, -1 = n/a                      */
    int     gpu_freq_mhz;
    int     disk_temp_c10;/* hottest disk, tenths of °C, -1 = n/a        */
    int     ncores;
    int     core[SLH_HIST_MAX_CORES];
} hist_record_t;

/** Fills one sample; returns 0 on success, -1 to skip this tick. */
typedef int (*hist_source_fn)(void *user, hist_sample_t *out);

/**
 * Open (or create) the ring file and start the one-second sampler thread.
 * Returns 0 on success. A failure to open the file is logged and disables
 * history without stopping the daemon.
 */
int  history_start(const config_t *cfg, hist_source_fn src, void *user);
void history_stop(void);

/** 1 when history is running and has at least one record. */
int  history_ready(void);

/** Oldest and newest stored timestamps; 0 when empty. */
void history_range(int64_t *from, int64_t *to);

/**
 * Copy up to `max` records with `from <= ts <= to`, oldest first.
 * `from`/`to` of 0 mean "no bound on that side". Returns the count copied.
 *
 * When the requested span holds more than `max` records the result is
 * decimated evenly so the caller still sees the full range.
 */
size_t history_query(int64_t from, int64_t to, hist_record_t *out, size_t max,
                     int *decimated);

/** Records currently stored and the ring capacity. */
void history_stats(size_t *count, size_t *capacity, int *interval_sec);

/** Bytes currently occupied by the ring file. */
uint64_t history_disk_bytes(void);

#endif /* SLH_HISTORY_H */
