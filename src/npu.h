/*
 * npu.h - NPU presence, driver state and load.
 *
 * Rockchip's RKNPU driver is the primary target, but the collector is written
 * as a chain of independent probes so it degrades honestly on kernels that
 * ship without the driver (many mainline/ophub builds do):
 *
 *   1. /sys/kernel/debug/rknpu/{load,freq,version}   - vendor driver, richest
 *   2. a /sys/class/devfreq node whose name mentions the NPU  - generic devfreq
 *   3. device tree / /dev/rknpu*                     - hardware presence only
 *
 * When only step 3 succeeds the card reports "驱动未加载" instead of inventing
 * numbers, and `note` carries a concrete hint for enabling it.
 */
#ifndef SLH_NPU_H
#define SLH_NPU_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SLH_MAX_NPU_CORES 8

typedef enum {
    NPU_STATE_UNKNOWN = 0,   /* nothing detected at all                    */
    NPU_STATE_HW_ONLY,       /* silicon present, driver not loaded         */
    NPU_STATE_READY          /* driver loaded; load may or may not be live */
} npu_state_t;

typedef struct {
    npu_state_t state;
    int         hw_present;
    int         driver_loaded;
    int         load_available;

    char name[64];              /* "RKNPU" when the vendor driver answered   */
    char hw_name[96];           /* "Rockchip RK3568 NPU"                     */
    char driver_version[64];
    char source[160];           /* path that produced the reading            */
    char note[224];             /* human hint when not fully ready           */

    double load_percent;        /* mean across cores, -1 when unknown        */
    double core_load[SLH_MAX_NPU_CORES];
    int    core_count;

    uint64_t freq_hz;
    uint64_t freq_min_hz;
    uint64_t freq_max_hz;
} npu_t;

/** Probe everything; never fails, always leaves `npu` in a describable state. */
void npu_collect(npu_t *npu);

void npu_to_json(strbuf_t *sb, const npu_t *npu, int *first);

#endif /* SLH_NPU_H */
