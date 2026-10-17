/*
 * updater.c - download, verify and stage a release, then hand it to root.
 *
 * See updater.h for why the privileged half lives in a separate systemd unit.
 */
#include "updater.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "httpclient.h"
#include "log.h"
#include "sha256.h"
#include "util.h"
#include "version.h"

#define UPDATE_TIMEOUT_MS 180000     /* 3 min for the tarball            */
#define MANIFEST_TIMEOUT_MS 15000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_thread;
static int             g_thread_live;

static const config_t *g_cfg;
static updater_status_t g_st;
static char             g_dir[640];       /* <state>/update               */
static char             g_pkg[700];       /* <state>/update/<ver>.tar.gz   */
static char             g_staged[700];    /* <state>/update/staged         */
static char             g_ready[700];     /* <state>/update/ready          */

/* ------------------------------------------------------------------ */
/* Small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path)
{
    char   tmp[700];
    char  *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int path_is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_is_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void rm_rf(const char *path)
{
    char tool[512];

    if (!slh_find_program("rm", tool, sizeof(tool)))
        return;
    {
        const char *argv[] = { tool, "-rf", path, NULL };

        (void)slh_run_command(argv, NULL, 0, 30000);
    }
}

static void set_status(int phase, const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&g_lock);
    g_st.phase = phase;
    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(g_st.message, sizeof(g_st.message), fmt, ap);
        va_end(ap);
    }
    if (phase != UPD_DOWNLOADING && phase != UPD_IDLE)
        g_st.percent = (phase == UPD_STAGED) ? 100 : -1;
    pthread_mutex_unlock(&g_lock);
}

const char *updater_phase_name(int phase)
{
    switch (phase) {
    case UPD_IDLE:        return "idle";
    case UPD_DOWNLOADING: return "downloading";
    case UPD_VERIFYING:   return "verifying";
    case UPD_EXTRACTING:  return "extracting";
    case UPD_STAGED:      return "staged";
    case UPD_FAILED:      return "failed";
    default:              return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* The worker                                                          */
/* ------------------------------------------------------------------ */

static int fail(const char *fmt, ...)
{
    va_list ap;
    char    msg[192];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_lock);
    g_st.phase = UPD_FAILED;
    slh_strlcpy(g_st.error, msg, sizeof(g_st.error));
    slh_strlcpy(g_st.message, msg, sizeof(g_st.message));
    g_st.finished_at = slh_now_sec();
    pthread_mutex_unlock(&g_lock);

    LOG_WARN("update: %s", msg);
    return -1;
}

/** Confirm the unpacked tree really is a Slhbond-Monitor source release. */
static int staged_looks_valid(const char *dir)
{
    static const char *const needed[] = {
        "deploy.sh", "Makefile", "src/main.c", "web/index.html", NULL
    };
    char   path[800];
    size_t i;

    for (i = 0; needed[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, needed[i]);
        if (!path_is_file(path)) {
            LOG_WARN("update: staged tree is missing %s", needed[i]);
            return 0;
        }
    }
    return 1;
}

static void *worker_main(void *arg)
{
    version_state_t vs;
    char            err[256];
    char            digest[SLH_SHA256_HEX_LEN];
    char            tool[512];
    int             rc;

    SLH_UNUSED(arg);

    version_state_get(&vs);

    /* --- 1. download -------------------------------------------------- */
    set_status(UPD_DOWNLOADING, "正在下载 %s", vs.latest);
    LOG_INFO("update: downloading %s from %s", vs.latest, vs.release_url);

    if (http_fetch_file(vs.release_url, g_pkg, UPDATE_TIMEOUT_MS,
                        err, sizeof(err)) != 0)
        return (void *)(intptr_t)fail("下载失败：%s", err);

    {
        struct stat st;

        if (stat(g_pkg, &st) == 0) {
            pthread_mutex_lock(&g_lock);
            g_st.bytes = (uint64_t)st.st_size;
            pthread_mutex_unlock(&g_lock);
        }
    }

    /* --- 2. verify ---------------------------------------------------- */
    set_status(UPD_VERIFYING, "正在校验完整性");

    if (sha256_file(g_pkg, digest, sizeof(digest)) != 0)
        return (void *)(intptr_t)fail("无法读取下载的文件");

    if (vs.release_sha256[0] == '\0') {
        /*
         * Refusing outright would make the feature unusable for people who
         * publish a manifest without a digest, so this is a loud warning
         * rather than a hard failure. It is spelled out in the UI and docs.
         */
        LOG_WARN("update: manifest has no sha256 - integrity NOT verified");
    } else if (!sha256_hex_equal(digest, vs.release_sha256)) {
        rm_rf(g_pkg);
        return (void *)(intptr_t)fail(
            "校验失败：下载文件的 SHA-256 与清单不符（期望 %.16s…，实际 %.16s…）",
            vs.release_sha256, digest);
    } else {
        LOG_INFO("update: sha256 verified (%s)", digest);
    }

    /* --- 3. extract --------------------------------------------------- */
    set_status(UPD_EXTRACTING, "正在解包");

    if (!slh_find_program("tar", tool, sizeof(tool)))
        return (void *)(intptr_t)fail("系统里没有 tar，无法解包");

    rm_rf(g_staged);
    if (mkdir_p(g_staged) != 0)
        return (void *)(intptr_t)fail("无法创建 %s", g_staged);

    {
        const char *argv[] = { tool, "-xzf", g_pkg, "-C", g_staged, NULL };

        rc = slh_run_command(argv, NULL, 0, 120000);
        if (rc != 0)
            return (void *)(intptr_t)fail("解包失败（tar 退出码 %d）", rc);
    }

    /*
     * Release tarballs have a single top-level directory (slhbond-monitor-X).
     * Collapse it so the root worker always sees the tree at a fixed path.
     */
    {
        char   top[700];
        char   entry[800];
        DIR   *d;
        struct dirent *e;
        int    only_one = 1;
        int    count = 0;

        top[0] = '\0';
        d = opendir(g_staged);
        if (d) {
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                count++;
                snprintf(top, sizeof(top), "%s", e->d_name);
            }
            closedir(d);
        }
        snprintf(entry, sizeof(entry), "%s/%s", g_staged, top);
        if (count != 1 || !path_is_dir(entry))
            only_one = 0;

        if (only_one) {
            char moved[800];

            snprintf(moved, sizeof(moved), "%s.__inner", g_staged);
            if (rename(entry, moved) == 0) {
                char flat[800];
                char inner[800];

                snprintf(flat, sizeof(flat), "%s", g_staged);
                snprintf(inner, sizeof(inner), "%s", moved);
                rm_rf(flat);
                if (rename(inner, flat) != 0)
                    return (void *)(intptr_t)fail("无法整理解包目录");
            }
        }
    }

    if (!staged_looks_valid(g_staged))
        return (void *)(intptr_t)fail("解包内容不像一个 Slhbond-Monitor 发布包");

    /* --- 4. publish --------------------------------------------------- */
    {
        FILE *fp = fopen(g_ready, "w");

        if (!fp)
            return (void *)(intptr_t)fail("无法写入 %s", g_ready);
        fprintf(fp, "%s\n", vs.latest);
        fclose(fp);
    }

    pthread_mutex_lock(&g_lock);
    slh_strlcpy(g_st.version, vs.latest, sizeof(g_st.version));
    g_st.bytes = 0;
    g_st.percent = 100;
    g_st.finished_at = slh_now_sec();
    pthread_mutex_unlock(&g_lock);

    set_status(UPD_STAGED, "%s 已下载并校验完成，等待系统应用", vs.latest);
    LOG_INFO("update: %s staged at %s, waiting for the root worker",
             vs.latest, g_staged);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int updater_start(const config_t *cfg)
{
    g_cfg = cfg;

    snprintf(g_dir, sizeof(g_dir), "%s/update", cfg->state_dir);
    snprintf(g_staged, sizeof(g_staged), "%s/staged", g_dir);
    snprintf(g_ready, sizeof(g_ready), "%s/ready", g_dir);
    g_pkg[0] = '\0';

    if (mkdir_p(g_dir) != 0)
        LOG_WARN("update: cannot create %s: %s", g_dir, strerror(errno));

    memset(&g_st, 0, sizeof(g_st));
    g_st.phase = UPD_IDLE;
    g_st.percent = -1;

    /* A leftover ready flag from a previous run still counts as staged. */
    if (path_is_file(g_ready)) {
        char ver[64];

        ver[0] = '\0';
        if (slh_read_text(g_ready, ver, sizeof(ver))) {
            slh_strlcpy(g_st.version, ver, sizeof(g_st.version));
            g_st.phase = UPD_STAGED;
            slh_strlcpy(g_st.message, "有已校验的更新等待应用", sizeof(g_st.message));
            g_st.percent = 100;
            LOG_INFO("update: %s is staged and still waiting to be applied", ver);
        }
    }
    return 0;
}

void updater_stop(void)
{
    if (g_thread_live) {
        pthread_join(g_thread, NULL);
        g_thread_live = 0;
    }
}

int updater_apply_async(char *err, size_t err_size)
{
    version_state_t vs;
    int             phase;

    version_state_get(&vs);

    if (!vs.update_available) {
        snprintf(err, err_size, "当前没有可用的新版本（当前 %s）", vs.current);
        return -1;
    }
    if (vs.release_url[0] == '\0') {
        snprintf(err, err_size, "清单里没有 url 字段，无法下载");
        return -1;
    }
    if (!slh_starts_with(vs.release_url, "http://") &&
        !slh_starts_with(vs.release_url, "https://")) {
        snprintf(err, err_size, "只支持 http/https 的下载地址");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    phase = g_st.phase;
    pthread_mutex_unlock(&g_lock);

    if (phase == UPD_DOWNLOADING || phase == UPD_VERIFYING ||
        phase == UPD_EXTRACTING) {
        snprintf(err, err_size, "已经有一个更新任务在进行中");
        return -1;
    }

    if (g_thread_live) {
        pthread_join(g_thread, NULL);
        g_thread_live = 0;
    }

    {
        /* Version strings are short, but bound the component anyway so the
         * compiler can prove the buffer is large enough. */
        char safe[48];

        slh_strlcpy(safe, vs.latest[0] ? vs.latest : "release", sizeof(safe));
        snprintf(g_pkg, sizeof(g_pkg), "%s/%s.tar.gz", g_dir, safe);
    }
    rm_rf(g_ready);

    pthread_mutex_lock(&g_lock);
    memset(&g_st, 0, sizeof(g_st));
    g_st.phase = UPD_DOWNLOADING;
    g_st.percent = 0;
    slh_strlcpy(g_st.version, vs.latest, sizeof(g_st.version));
    slh_strlcpy(g_st.message, "正在准备下载", sizeof(g_st.message));
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0) {
        snprintf(err, err_size, "无法启动下载线程");
        return -1;
    }
    g_thread_live = 1;
    return 0;
}

void updater_status_get(updater_status_t *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);

    /* Progress is derived from the file on disk: the downloader is a child
     * process, so there is no byte counter to read from it. */
    if (out->phase == UPD_DOWNLOADING && g_pkg[0]) {
        struct stat st;

        if (stat(g_pkg, &st) == 0) {
            version_state_t vs;

            version_state_get(&vs);
            out->bytes = (uint64_t)st.st_size;
            if (vs.release_size > 0 && st.st_size <= vs.release_size) {
                out->percent = (int)((int64_t)st.st_size * 100 / vs.release_size);
                if (out->percent > 99)
                    out->percent = 99;
            } else {
                out->percent = -1;
            }
        }
    }
}

int updater_has_staged(void)
{
    return path_is_file(g_ready);
}

void updater_staged_path(char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    if (path_is_file(g_ready))
        slh_strlcpy(out, g_staged, out_size);
    else
        out[0] = '\0';
}
