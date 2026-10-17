/*
 * updater.h - download, verify and stage a release, then hand it to root.
 *
 * Why it is split in two:
 *
 *   this service             runs as a DynamicUser with no privileges, so it
 *                            can download and verify but cannot write to
 *                            /opt or restart systemd units.
 *   slhbond-update.service   a oneshot unit that runs as root on a timer. It
 *                            only does anything when a staged release is
 *                            waiting, and it re-checks everything itself.
 *
 * The service therefore never gets the ability to install anything: it can
 * only put a *verified* tarball in a directory that the root worker watches.
 *
 * Integrity: the manifest carries the expected SHA-256, and the download is
 * only staged once the digest matches. Without that step a self-update would
 * be a remote-code-execution hole, since the transport is only as trustworthy
 * as the system's CA store.
 */
#ifndef SLH_UPDATER_H
#define SLH_UPDATER_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    UPD_IDLE = 0,        /* nothing in flight                          */
    UPD_DOWNLOADING,
    UPD_VERIFYING,
    UPD_EXTRACTING,
    UPD_STAGED,          /* verified and unpacked, waiting for the root worker */
    UPD_FAILED
} updater_phase_t;

typedef struct {
    int      phase;
    char     version[64];
    char     message[224];
    char     error[192];
    int      percent;        /* 0..100 while downloading, -1 when unknown   */
    uint64_t bytes;          /* bytes on disk so far                        */
    int64_t  finished_at;    /* epoch seconds, 0 while still running        */
} updater_status_t;

int  updater_start(const config_t *cfg);
void updater_stop(void);

/**
 * Begin download + verify + stage in the background.
 * Returns 0 when the job was started, -1 with `err` set otherwise (no update
 * available, missing url, or one already in flight).
 */
int  updater_apply_async(char *err, size_t err_size);

void updater_status_get(updater_status_t *out);

/** 1 when a verified release is staged and the root worker has not taken it. */
int  updater_has_staged(void);

/** Absolute path of the staged tree ("" when nothing is staged). */
void updater_staged_path(char *out, size_t out_size);

/** "idle" / "downloading" / ... — for the JSON API. */
const char *updater_phase_name(int phase);

#endif /* SLH_UPDATER_H */
