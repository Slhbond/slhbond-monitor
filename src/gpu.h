/*
 * gpu.h - GPU presence, frequency, thermal state and activity.
 *
 * On the Rockchip boards this targets, the GPU is a Mali part driven by
 * panfrost. Unlike the vendor mali kbase driver, panfrost exposes **no**
 * utilisation counter, so this module never invents one:
 *
 *   load_percent        stays -1.0, and the API says so
 *   active_ratio_percent  the share of wall time the GPU was powered on,
 *                         derived from power/runtime_active_time deltas
 *   freq_*              current / min / max / average operating point
 *
 * The dashboard renders the frequency operating point and the power-on share
 * rather than pretending to know a utilisation percentage.
 */
#ifndef SLH_GPU_H
#define SLH_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SLH_MAX_GPU_FREQ_STEPS 16

typedef struct {
    int  present;
    char model[96];           /* "Mali-G52 2EE"                          */
    char compatible[160];     /* raw device-tree compatibles             */
    char driver[48];          /* "panfrost"                              */
    char render_node[64];     /* "/dev/dri/renderD128" or ""             */
    char devfreq_name[96];

    uint64_t freq_cur_hz;
    uint64_t freq_min_hz;
    uint64_t freq_max_hz;
    uint64_t freq_avg_hz;     /* lifetime average from trans_stat, 0 = n/a */
    uint64_t freq_steps[SLH_MAX_GPU_FREQ_STEPS];
    size_t   freq_step_count;
    int      freq_step_index; /* index of freq_cur in freq_steps, -1 = n/a */

    char governor[32];
    char power_state[24];     /* "active" / "suspended" / ""             */

    double temp_c;            /* -1.0 when no sensor is exposed          */
    char   temp_label[48];

    double load_percent;      /* always -1.0 unless the kernel offers one */
    char   load_source[96];

    double   active_ratio_percent;  /* -1.0 until a second sample exists */
    uint64_t active_ms;
} gpu_t;

/** Sample the GPU. Always leaves `g` usable; check `present`. */
void gpu_collect(gpu_t *g);

void gpu_to_json(strbuf_t *sb, const gpu_t *g, int *first);

#endif /* SLH_GPU_H */
