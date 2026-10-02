/*
 * version.h - build version and the background "update available" checker.
 *
 * The dashboard shows a blue «更新可用» button only while the manifest it
 * polls advertises a version newer than the running build.
 */
#ifndef SLH_VERSION_H
#define SLH_VERSION_H

#include "config.h"
#include "util.h"

/* Injected by the Makefile from the VERSION file; keep a sane fallback so the
 * tree still builds when someone compiles a single file by hand. */
#ifndef SLH_VERSION_STRING
#  define SLH_VERSION_STRING "0.0.0-dev"
#endif

typedef struct {
    char current[64];        /* version this process was built from        */
    char latest[64];         /* newest version advertised by the manifest  */
    int  update_available;
    int  enabled;            /* update checking turned on in config        */
    int  last_check_ok;
    char release_url[512];
    char notes[512];
    char source[512];        /* manifest path or URL that was polled       */
    char checked_at[48];     /* RFC3339 local time of the last attempt     */
    char last_error[192];
} version_state_t;

/** Compare dotted versions. Returns <0, 0 or >0 (semver-ish ordering). */
int version_compare(const char *a, const char *b);

/** Seed the shared state from the running build. Call once before threads. */
void version_state_init(void);

/** Thread-safe snapshot of the current state. */
void version_state_get(version_state_t *out);

/**
 * Poll the configured manifest once and update the shared state.
 * Returns 0 when the manifest was read and parsed, -1 otherwise.
 */
int version_check_now(void);

/** Start the background poller. */
void version_checker_start(const config_t *cfg);

/** Signal the poller to stop and wait for it. */
void version_checker_stop(void);

/** Emit the /api/v1/version payload. */
void version_to_json(strbuf_t *sb, const version_state_t *st, const config_t *cfg);

#endif /* SLH_VERSION_H */
