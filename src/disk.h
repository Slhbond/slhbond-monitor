/*
 * disk.h - block-device enumeration and on-demand health checking.
 *
 * Enumeration is cheap (sysfs + /proc/mounts only) and safe to call on every
 * request. Health checking is expensive - it may exec smartctl - so it runs on
 * a dedicated worker thread and the results are cached by device name.
 *
 * The dashboard's contract, mirroring the product requirement:
 *   - a disk that has never been seen gets ONE automatic check;
 *   - after that, only an explicit request triggers another one.
 * Which devices already had their automatic check is persisted under
 * cfg->state_dir so a restart does not re-run it.
 */
#ifndef SLH_DISK_H
#define SLH_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SLH_MAX_DISKS 16

/**
 * Rolling window of throughput samples kept per disk.
 *
 * Disk I/O is bursty: on an idle board the instantaneous rate is 0 for most
 * five-second windows even though hundreds of kilobytes are written every
 * minute. Keeping a short history lets the card draw a sparkline so those
 * bursts are actually visible, instead of the panel looking dead.
 */
#define SLH_DISK_IO_HISTORY 32

typedef enum {
    DISK_HEALTH_UNKNOWN = 0,
    DISK_HEALTH_HEALTHY = 1,
    DISK_HEALTH_WARNING = 2,
    DISK_HEALTH_FAULT   = 3
} disk_health_t;

typedef struct {
    /* --- identity -------------------------------------------------- */
    char device[32];          /* "sda", "mmcblk1"                        */
    char name[96];            /* model, or the eMMC product name         */
    char kind[24];            /* "SSD" / "HDD" / "eMMC" / "SD 卡" / ...  */
    char transport[16];       /* "sata" / "mmc" / "nvme" / "usb"         */
    char vendor[64];
    char serial[64];
    char firmware[64];
    int  removable;
    int  rotational;
    uint64_t size_bytes;

    /* --- filesystem, when a partition of this disk is mounted ----- */
    int      mounted;
    char     mount_point[192];
    char     fs_type[24];
    char     fs_device[64];
    /**
     * 1 when this disk carries an operating-system mount ("/", "/usr",
     * "/var", "/etc", "/boot", ...). Used only for the 系统盘 badge.
     */
    int      is_system;
    /** The system mount found on this disk, e.g. "/" - empty otherwise. */
    char     system_mount[64];
    /**
     * NOTE: under systemd's ProtectSystem=strict this is *always* 1, because
     * the service's own mount namespace is remounted read-only. It is kept as
     * informational output only - it must never drive the health verdict, or
     * every disk would be permanently reported as degraded.
     */
    int      fs_read_only;
    uint64_t fs_total_bytes;
    uint64_t fs_free_bytes;
    uint64_t fs_used_bytes;
    double   fs_used_percent;
    /** Filesystem-level error counter (ext4 exposes this in sysfs). Unlike
     *  the mount flags it means the same thing inside and outside the
     *  service's mount namespace. */
    uint64_t fs_errors_count;

    /* --- i/o since boot -------------------------------------------- */
    uint64_t read_bytes;
    uint64_t write_bytes;
    /* --- instantaneous throughput, from /sys/block/X/stat deltas ---- */
    double   read_bps;         /* bytes/s since the previous sample  */
    double   write_bps;
    double   read_peak_bps;
    double   write_peak_bps;
    double   read_avg_bps;     /* mean over the rolling window        */
    double   write_avg_bps;

    int      io_hist_count;    /* valid entries, <= SLH_DISK_IO_HISTORY */
    int      io_hist_head;     /* ring write index                      */
    double   read_hist[SLH_DISK_IO_HISTORY];
    double   write_hist[SLH_DISK_IO_HISTORY];

    /* --- health ----------------------------------------------------- */
    int           health_checked;
    disk_health_t health;
    char          health_source[40];   /* "eMMC" / "smartctl" / "文件系统" */
    char          health_summary[192];
    char          health_detail[768];
    char          health_checked_at[40];
    int           health_auto;
    double        temperature_c;       /* -1 when unknown */
    uint64_t      power_on_hours;
} disk_info_t;

typedef struct {
    size_t      count;
    disk_info_t disk[SLH_MAX_DISKS];
    int         check_in_progress;
    char        check_device[32];
} disk_list_t;

/** Start the background health-check worker. `state_dir` may be NULL. */
void disk_init(const char *state_dir);

/** Stop the worker and flush the auto-checked record. */
void disk_shutdown(void);

/** Enumerate disks and merge in whatever health results are already known. */
void disk_collect(disk_list_t *out);

/**
 * Queue a health check for `device`.
 * Returns 0 when queued, -1 when the queue is unavailable or the device is
 * unknown.
 */
int disk_request_check(const char *device);

/** 1 when `device` matches one of the enumerated disks. */
int disk_device_exists(const char *device);

void disk_to_json(strbuf_t *sb, const disk_info_t *d, int *first);

/** Human-readable name for a health level. */
const char *disk_health_name(disk_health_t level);

#endif /* SLH_DISK_H */
