/*
 * cmdlog.h - record what users actually run, with deletions called out.
 *
 * Two sources, because neither is complete on its own:
 *
 *   进程扫描   every second we diff /proc against the previous snapshot and
 *              record the command line of anything new. This catches commands
 *              driven from scripts, cron and ssh one-liners, but a process
 *              that starts and exits inside the same second is invisible.
 *
 *   历史文件   ~/.bash_history and ~/.zsh_history, tailed from a saved offset.
 *              This catches exactly the fast interactive commands the process
 *              scan misses, but only appears once the shell flushes the file.
 *
 * Neither needs extra privileges. What they cannot see is stated plainly in
 * the UI rather than being papered over: shell builtins that never fork, and
 * commands run while the service was stopped.
 *
 * Deletions (rm / rmdir / unlink / shred / truncate, including `sh -c "rm .."`)
 * are flagged so the panel can list them separately - that is the case that
 * matters most when something has gone missing.
 */
#ifndef SLH_CMDLOG_H
#define SLH_CMDLOG_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define SLH_MAX_COMMANDS 500

typedef enum {
    CMD_SOURCE_PROC = 0,     /* seen live in /proc                      */
    CMD_SOURCE_HISTORY = 1   /* read back from a shell history file     */
} cmd_source_t;

typedef struct {
    int64_t ts;
    int     source;          /* cmd_source_t                            */
    int     uid;
    int     pid;
    int     ppid;
    int     is_delete;       /* 删除类操作                              */
    char    user[40];
    char    cmd[512];
    char    target[256];     /* 删除的目标（第一个非选项参数）           */
} cmd_record_t;

/** Open the log and prime the /proc snapshot. Returns 0 on success. */
int  cmdlog_start(const config_t *cfg);
void cmdlog_stop(void);

/** Called periodically from the sampler thread. */
void cmdlog_poll(void);

/** Copy the newest `max` records (newest first). Returns how many. */
size_t cmdlog_list(cmd_record_t *out, size_t max);

/** Newest `max` records with is_delete set. */
size_t cmdlog_list_deletes(cmd_record_t *out, size_t max);

/** How many deletion records are currently held. */
size_t cmdlog_delete_count(void);

uint64_t cmdlog_disk_bytes(void);

/** Total records held. */
size_t cmdlog_total(void);

/** 1 when the shell-history source is readable; `why` explains when not. */
int cmdlog_history_status(char *why, size_t why_size);

#endif /* SLH_CMDLOG_H */
