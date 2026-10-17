/*
 * events.h - machine power-cycle history plus SSH logins.
 *
 * "开机" and "关机" mean the *machine*, not this service. The distinction
 * matters: restarting the monitor is not a power cycle, and reporting it as
 * one makes the log useless for answering "when did the box actually go down".
 *
 * The kernel changes /proc/sys/kernel/random/boot_id on every boot, so that
 * value is the test:
 *
 *   开机    recorded only when boot_id differs from the one we last saw.
 *           The time comes from /proc/uptime, so it is the real kernel start.
 *   关机    systemd signals us on the way down, but the same signal arrives
 *           for a plain service restart. So the stop is recorded as a *service*
 *           stop, and only promoted to a machine shutdown once the next start
 *           proves the machine really rebooted. A power cut leaves no stop
 *           record at all, and is reported as such rather than invented.
 *   服务启停 recorded separately and hidden by default, so a restart cannot
 *           be mistaken for the machine going down.
 *
 * Logins come from the utmp database (/var/log/wtmp), a binary record file;
 * <utmp.h> gives us the platform's own layout so this works on 32- and 64-bit
 * alike. A byte offset is kept so each scan only reads what is new.
 */
#ifndef SLH_EVENTS_H
#define SLH_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define SLH_MAX_EVENTS 300

typedef enum {
    EVENT_BOOT = 0,           /* 整机开机                                  */
    EVENT_SHUTDOWN = 1,       /* 整机关机                                  */
    EVENT_LOGIN = 2,
    EVENT_LOGOUT = 3,
    EVENT_SERVICE_START = 4,  /* 监控服务启动（整机没重启时才会单独出现）   */
    EVENT_SERVICE_STOP = 5    /* 监控服务停止                              */
} event_type_t;

/** 1 for the two types that mean the machine itself went up or down. */
#define EVENT_IS_POWER(t) ((t) == EVENT_BOOT || (t) == EVENT_SHUTDOWN)

typedef struct {
    int64_t ts;
    int     type;          /* event_type_t                                 */
    int     unclean;       /* shutdown only: no clean stop was recorded     */
    char    user[64];
    char    ip[80];
    char    detail[96];
} event_record_t;

/** Open the log, decide whether the machine really rebooted, scan logins. */
int  events_start(const config_t *cfg);

/** Record this service's own stop. Call before the process exits. */
void events_stop(void);

/** Re-scan the login database; safe to call periodically. */
void events_poll(void);

/**
 * Copy the newest `max` records (newest first).
 * When `include_service` is 0 the service start/stop rows are skipped, so the
 * list only shows machine power cycles and logins.
 */
size_t events_list(event_record_t *out, size_t max, int include_service);

/** 0 when logins are being tracked, -1 otherwise; `why` explains the failure. */
int  events_login_status(char *why, size_t why_size);

/** Boot time of the current session, 0 when unknown. */
int64_t events_boot_time(void);

/** 1 when the last start proved the machine had rebooted. */
int  events_is_new_boot(void);

uint64_t events_disk_bytes(void);

/** "开机" / "关机" / "登录" / "登出" / "服务启动" / "服务停止" */
const char *events_type_name(int type);

#endif /* SLH_EVENTS_H */
