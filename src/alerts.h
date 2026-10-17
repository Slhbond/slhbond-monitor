/*
 * alerts.h - threshold breaches recorded to disk.
 *
 * A metric is only recorded once it has stayed above its threshold for
 * `hold_sec` consecutive samples, and it is only considered "back to normal"
 * once it has dropped clearly below the threshold again. Without that
 * hysteresis a value hovering on the line would produce one record per
 * sample and fill the log within minutes.
 *
 * Records are bounded (SLH_MAX_ALERTS) and the log file is rewritten only
 * when an alert opens or closes, so the disk cost is negligible.
 */
#ifndef SLH_ALERTS_H
#define SLH_ALERTS_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "history.h"

#define SLH_MAX_ALERTS 200

typedef enum {
    ALERT_METRIC_CPU = 0,
    ALERT_METRIC_MEM = 1,
    ALERT_METRIC_TEMP = 2,
    ALERT_METRIC_GPU = 3,
    ALERT_METRIC_NPU = 4,
    ALERT_METRIC_DISK_TEMP = 5,
    ALERT_METRIC_COUNT = 6
} alert_metric_t;

typedef struct {
    int64_t ts;            /* when the breach started                     */
    int64_t ended_ts;      /* when it dropped back; 0 = still ongoing     */
    int     metric;        /* alert_metric_t                              */
    int     interrupted;   /* ended because the service stopped, not
                            * because the value came back down            */
    double  peak;          /* worst value seen during the breach          */
    double  threshold;     /* the limit that was crossed                  */
    int     duration_sec;  /* length so far (or total, once ended)        */
} alert_record_t;

/**
 * Live state of one metric, so the panel can show *why* nothing has been
 * recorded yet ("已超限 8 秒 / 需要 10 秒") instead of looking broken.
 */
typedef struct {
    int    metric;
    int    available;      /* the board reports it at all                 */
    int    active;         /* a record is already open                    */
    double current;        /* newest sample, -1 when unavailable          */
    double threshold;
    double pending_peak;   /* worst value of the current run              */
    int    pending_sec;    /* seconds above the line so far, 0 = normal   */
    int    hold_sec;       /* seconds required before a record opens      */
} alert_metric_state_t;

typedef struct {
    double cpu_percent;
    double mem_percent;
    double temp_c;
    double gpu_percent;    /* GPU 上电占比（panfrost 没有利用率可用） */
    double npu_percent;    /* NPU 负载，驱动未加载时不参与判断       */
    double disk_temp_c;    /* 所有硬盘里最高的那块                   */
    int    hold_sec;       /* seconds above the line before recording */
} alert_thresholds_t;

/** Load persisted thresholds and open the log. Returns 0 on success. */
int  alerts_start(const config_t *cfg);
void alerts_stop(void);

/** Evaluate one sample; called from the history sampler thread. */
void alerts_evaluate(const hist_sample_t *s);

/** Copy the newest `max` records (newest first). Returns how many. */
size_t alerts_list(alert_record_t *out, size_t max);

/** Live per-metric state, in alert_metric_t order. Returns how many. */
size_t alerts_metric_states(alert_metric_state_t *out, size_t max);

/** How many breaches are ongoing right now. */
int  alerts_active_count(void);

/** Total number of records currently held. */
size_t alerts_total(void);

uint64_t alerts_disk_bytes(void);

void alerts_get_thresholds(alert_thresholds_t *out);

/**
 * Replace the thresholds and persist them next to the other runtime state.
 * Returns 0 on success, -1 with `err` filled in when a value is out of range.
 */
int  alerts_set_thresholds(const alert_thresholds_t *in, char *err, size_t err_size);

/** "CPU 使用率" / "内存使用率" / "CPU 温度" / ... */
const char *alerts_metric_name(int metric);

/**
 * 1 when this metric can actually be measured right now.
 *
 * GPU utilisation and NPU load are unavailable on many boards (panfrost
 * exports no counter, the RKNPU driver may not be loaded), and a metric that
 * is always 0 must never look like "fine".
 */
int alerts_metric_available(int metric);

#endif /* SLH_ALERTS_H */
