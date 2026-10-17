/*
 * cmdlog.c - record what users actually run, with deletions called out.
 *
 * See cmdlog.h. Both sources are diff-based: /proc is compared against the
 * previous second's snapshot, and history files are read from a byte offset,
 * so a poll costs almost nothing and never re-reads what it has already seen.
 */
#include "cmdlog.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define CMDLOG_MAX_PIDS   512
#define CMDLOG_MAX_HOMES  24
#define CMDLOG_MAX_HIST   48

typedef struct {
    char  path[320];
    off_t off;
    int   primed;
} hist_file_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cmd_record_t    g_rec[SLH_MAX_COMMANDS];
static size_t          g_count;      /* valid entries, newest last */
static size_t          g_head;       /* ring write index           */
static size_t          g_deletes;    /* how many of them are deletions */
static char            g_log_path[640];
static char            g_off_path[640];
static uint64_t        g_disk_bytes;

static pid_t           g_seen[CMDLOG_MAX_PIDS];
static size_t          g_seen_count;
static int             g_primed;

/*
 * How many data lines the log file currently holds. Appending keeps the cost
 * of a new command at one short write; the file is only rewritten when it has
 * grown to twice the ring, so the on-disk size stays bounded without rewriting
 * hundreds of lines for every process that starts.
 */
static size_t          g_lines_on_disk;
static size_t          g_compact_turn;

/* getpwuid() can hit NSS and is not cheap; command capture is high rate. */
#define UID_CACHE_N 24
static struct {
    int  uid;
    char name[40];
} g_uid_cache[UID_CACHE_N];
static size_t          g_uid_cache_count;
static size_t          g_uid_cache_turn;

static hist_file_t     g_hist[CMDLOG_MAX_HIST];
static size_t          g_hist_count;
static int             g_hist_ok;
static char            g_hist_why[160];

/* ------------------------------------------------------------------ */
/* Deletion detection                                                  */
/* ------------------------------------------------------------------ */

static int is_delete_name(const char *tok, size_t len)
{
    static const char *names[] = {
        "rm", "rmdir", "unlink", "shred", "truncate", "srm", NULL
    };
    int i;

    for (i = 0; names[i]; i++) {
        if (strlen(names[i]) == len && strncmp(tok, names[i], len) == 0)
            return 1;
    }
    return 0;
}

/**
 * Decide whether `cmd` is a deletion and, if so, pick out the first target.
 *
 * Handles the direct form ("rm -rf /tmp/x") and the wrapped form
 * ("sh -c rm -rf /tmp/x"), which is how most scripted deletions look in
 * /proc/<pid>/cmdline. Any token is accepted as the verb, so a wrapper does
 * not hide it.
 */
static int detect_delete(const char *cmd, char *target, size_t target_size)
{
    const char *p;
    int         found = 0;

    target[0] = '\0';

    /* Pass 1: does any token name a delete tool? */
    for (p = cmd; *p; ) {
        const char *start;
        const char *base;
        size_t      len;
        size_t      k;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        len = (size_t)(p - start);

        base = start;
        for (k = 0; k < len; k++) {
            if (start[k] == '/')
                base = start + k + 1;
        }
        if (is_delete_name(base, (size_t)(start + len - base))) {
            found = 1;
            break;
        }
    }
    if (!found)
        return 0;

    /* Pass 2: first non-option argument after the verb is the target. */
    found = 0;
    for (p = cmd; *p; ) {
        const char *start;
        const char *base;
        size_t      len;
        size_t      k;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        len = (size_t)(p - start);

        base = start;
        for (k = 0; k < len; k++) {
            if (start[k] == '/')
                base = start + k + 1;
        }
        if (!found) {
            if (is_delete_name(base, (size_t)(start + len - base)))
                found = 1;
            continue;
        }
        if (start[0] == '-')
            continue;
        if (len >= target_size)
            len = target_size - 1;
        memcpy(target, start, len);
        target[len] = '\0';
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
    }
}

static const char *user_for_uid(int uid)
{
    size_t slot;
    size_t i;

    for (i = 0; i < g_uid_cache_count; i++) {
        if (g_uid_cache[i].uid == uid)
            return g_uid_cache[i].name;
    }

    if (g_uid_cache_count < UID_CACHE_N) {
        slot = g_uid_cache_count++;
    } else {
        slot = g_uid_cache_turn++ % UID_CACHE_N;
    }

    g_uid_cache[slot].uid = uid;
    {
        struct passwd *pw = getpwuid((uid_t)uid);

        slh_strlcpy(g_uid_cache[slot].name, pw ? pw->pw_name : "-",
                    sizeof(g_uid_cache[slot].name));
    }
    return g_uid_cache[slot].name;
}

static void write_record(FILE *fp, const cmd_record_t *r)
{
    fprintf(fp, "%lld\t%s\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n",
            (long long)r->ts,
            r->source == CMD_SOURCE_HISTORY ? "history" : "proc",
            r->uid, r->pid, r->ppid, r->is_delete,
            r->user, r->cmd, r->target);
}

/** Full rewrite of the newest records, used at start-up and when compacting. */
static void save_locked(void)
{
    FILE *fp;
    size_t i;

    if (g_log_path[0] == '\0')
        return;
    fp = fopen(g_log_path, "w");
    if (!fp) {
        LOG_WARN("cannot write %s: %s", g_log_path, strerror(errno));
        return;
    }
    fprintf(fp, "# epoch\tsource\tuid\tpid\tppid\tdelete\tuser\tcmd\ttarget\n");
    for (i = 0; i < g_count; i++) {
        size_t idx = (g_head + SLH_MAX_COMMANDS - g_count + i) % SLH_MAX_COMMANDS;

        write_record(fp, &g_rec[idx]);
    }
    if (fclose(fp) == 0) {
        struct stat st;

        g_lines_on_disk = g_count;
        if (stat(g_log_path, &st) == 0)
            g_disk_bytes = (uint64_t)st.st_size;
    }
}

/**
 * Append the `added` newest records.
 *
 * This is the hot path: it runs every time a process starts, several times a
 * second on a busy machine. Rewriting the whole ring here was the single
 * biggest CPU cost of the daemon, so only the new rows are written and the
 * file is compacted once it has doubled.
 */
static void append_locked(int added)
{
    FILE *fp;
    int   i;

    if (added <= 0 || g_log_path[0] == '\0')
        return;

    fp = fopen(g_log_path, "a");
    if (!fp) {
        LOG_WARN("cannot append to %s: %s", g_log_path, strerror(errno));
        return;
    }
    if (g_lines_on_disk == 0)
        fprintf(fp, "# epoch\tsource\tuid\tpid\tppid\tdelete\tuser\tcmd\ttarget\n");

    for (i = added; i >= 1; i--) {
        size_t idx = (g_head + SLH_MAX_COMMANDS - (size_t)i) % SLH_MAX_COMMANDS;

        if (idx >= SLH_MAX_COMMANDS)
            continue;
        write_record(fp, &g_rec[idx]);
    }
    if (fclose(fp) == 0) {
        struct stat st;

        g_lines_on_disk += (size_t)added;
        if (stat(g_log_path, &st) == 0)
            g_disk_bytes = (uint64_t)st.st_size;
    }

    /* Keep the file from growing without bound. */
    if (g_lines_on_disk > SLH_MAX_COMMANDS * 2 || g_disk_bytes > 1024u * 1024u)
        save_locked();
    (void)g_compact_turn;
}

static void add_locked(int64_t ts, int source, int uid, int pid, int ppid,
                       const char *cmd)
{
    cmd_record_t *r = &g_rec[g_head];
    char          target[256];

    /* The slot about to be overwritten is the oldest one when the ring is
     * full; keep the deletion tally honest as records age out. */
    if (g_count == SLH_MAX_COMMANDS && g_rec[g_head].is_delete)
        g_deletes--;

    memset(r, 0, sizeof(*r));
    r->ts = ts;
    r->source = source;
    r->uid = uid;
    r->pid = pid;
    r->ppid = ppid;
    r->is_delete = detect_delete(cmd, target, sizeof(target));

    slh_strlcpy(r->user, user_for_uid(uid), sizeof(r->user));
    slh_strlcpy(r->cmd, cmd, sizeof(r->cmd));
    slh_strlcpy(r->target, target, sizeof(r->target));
    sanitize(r->cmd);
    sanitize(r->target);

    if (r->is_delete) {
        g_deletes++;
        LOG_INFO("command: %s ran a delete: %s%s%s", r->user, r->cmd,
                 r->target[0] ? "  ->  " : "", r->target);
    }

    g_head = (g_head + 1) % SLH_MAX_COMMANDS;
    if (g_count < SLH_MAX_COMMANDS)
        g_count++;
}

static void load_locked(void)
{
    FILE *fp;
    char  line[1400];

    fp = fopen(g_log_path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char          *fields[9];
        int            n = 0;
        char          *p = line;
        cmd_record_t   r;

        if (line[0] == '#')
            continue;
        memset(&r, 0, sizeof(r));
        while (n < 9) {
            char *tab = strchr(p, '\t');

            fields[n++] = p;
            if (!tab)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n < 9)
            continue;
        fields[8][strcspn(fields[8], "\r\n")] = '\0';
        r.ts = (int64_t)strtoll(fields[0], NULL, 10);
        r.source = strcmp(fields[1], "history") == 0 ? CMD_SOURCE_HISTORY
                                                     : CMD_SOURCE_PROC;
        r.uid = (int)strtol(fields[2], NULL, 10);
        r.pid = (int)strtol(fields[3], NULL, 10);
        r.ppid = (int)strtol(fields[4], NULL, 10);
        r.is_delete = (int)strtol(fields[5], NULL, 10);
        slh_strlcpy(r.user, fields[6], sizeof(r.user));
        slh_strlcpy(r.cmd, fields[7], sizeof(r.cmd));
        slh_strlcpy(r.target, fields[8], sizeof(r.target));

        if (r.is_delete)
            g_deletes++;
        g_rec[g_head] = r;
        g_head = (g_head + 1) % SLH_MAX_COMMANDS;
        if (g_count < SLH_MAX_COMMANDS)
            g_count++;
    }
    fclose(fp);
    if (g_count)
        LOG_INFO("commands: restored %zu record(s), %zu deletion(s)",
                 g_count, g_deletes);
}

static void load_offsets_locked(void)
{
    FILE *fp;
    char  line[420];

    fp = fopen(g_off_path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t');
        size_t i;

        if (!tab)
            continue;
        *tab = '\0';
        tab[strcspn(tab + 1, "\r\n")] = '\0';
        for (i = 0; i < g_hist_count; i++) {
            if (strcmp(g_hist[i].path, line) == 0) {
                g_hist[i].off = (off_t)strtoll(tab + 1, NULL, 10);
                g_hist[i].primed = 1;
                break;
            }
        }
    }
    fclose(fp);
}

static void save_offsets_locked(void)
{
    FILE *fp = fopen(g_off_path, "w");
    size_t i;

    if (!fp)
        return;
    for (i = 0; i < g_hist_count; i++)
        fprintf(fp, "%s\t%ld\n", g_hist[i].path, (long)g_hist[i].off);
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* /proc scanning                                                      */
/* ------------------------------------------------------------------ */

static int pid_seen(pid_t pid)
{
    size_t i;

    for (i = 0; i < g_seen_count; i++) {
        if (g_seen[i] == pid)
            return 1;
    }
    return 0;
}

/** Read /proc/<pid>/stat and return the parent pid (-1 on failure). */
static int read_ppid(const char *pid_str)
{
    char  path[64];
    char  buf[512];
    FILE *fp;
    char *close_paren;
    int   ppid = -1;

    snprintf(path, sizeof(path), "/proc/%s/stat", pid_str);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    if (fgets(buf, sizeof(buf), fp)) {
        /* comm can contain spaces and parentheses, so start after the LAST ')'. */
        close_paren = strrchr(buf, ')');
        if (close_paren && sscanf(close_paren + 2, "%*c %d", &ppid) != 1)
            ppid = -1;
    }
    fclose(fp);
    return ppid;
}

static int read_uid(const char *pid_str)
{
    char  path[64];
    char  buf[1024];
    FILE *fp;
    int   uid = -1;

    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strncmp(buf, "Uid:", 4) == 0) {
            if (sscanf(buf + 4, "%d", &uid) != 1)
                uid = -1;
            break;
        }
    }
    fclose(fp);
    return uid;
}

static void inspect_pid_locked(const char *pid_str, pid_t pid)
{
    char    path[64];
    char    raw[2048];
    ssize_t n;
    FILE   *fp;
    size_t  i;
    int     uid;
    int     ppid;
    int64_t now = slh_now_sec();

    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid_str);
    fp = fopen(path, "rb");
    if (!fp)
        return;
    n = (ssize_t)fread(raw, 1, sizeof(raw) - 1, fp);
    fclose(fp);
    if (n <= 0)
        return;                        /* kernel thread: no argv */
    raw[n] = '\0';

    /* argv is NUL-separated; show it as a normal command line. */
    for (i = 0; i < (size_t)n; i++) {
        if (raw[i] == '\0')
            raw[i] = ' ';
    }
    while (n > 0 && raw[n - 1] == ' ')
        raw[--n] = '\0';
    if (raw[0] == '\0')
        return;

    /* Skip ourselves and anything we spawn (smartctl, mmc, ...). */
    if (pid == getpid())
        return;
    ppid = read_ppid(pid_str);
    if (ppid == (int)getpid())
        return;

    uid = read_uid(pid_str);
    add_locked(now, CMD_SOURCE_PROC, uid, (int)pid, ppid, raw);
}

static int scan_procs_locked(void)
{
    DIR           *dir;
    struct dirent *entry;
    pid_t          now[CMDLOG_MAX_PIDS];
    size_t         now_count = 0;
    int            recording = g_primed;
    int            added = 0;

    dir = opendir("/proc");
    if (!dir)
        return 0;

    while ((entry = readdir(dir)) != NULL) {
        pid_t pid;

        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;
        pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0)
            continue;
        if (now_count < CMDLOG_MAX_PIDS)
            now[now_count++] = pid;
    }
    closedir(dir);

    if (recording) {
        size_t before = g_count;
        size_t i;

        for (i = 0; i < now_count; i++) {
            char pid_str[24];

            if (pid_seen(now[i]))
                continue;
            snprintf(pid_str, sizeof(pid_str), "%d", (int)now[i]);
            inspect_pid_locked(pid_str, now[i]);
        }
        added = (int)(g_count - before);
        if (g_count == SLH_MAX_COMMANDS && added == 0)
            added = 0;      /* ring was already full; nothing new appeared */
    }

    memcpy(g_seen, now, now_count * sizeof(now[0]));
    g_seen_count = now_count;
    g_primed = 1;
    return added;
}

/* ------------------------------------------------------------------ */
/* Shell history                                                       */
/* ------------------------------------------------------------------ */

static void discover_history_files_locked(void)
{
    FILE *fp;
    char  line[512];
    int   seen_files = 0;
    int   unreadable = 0;

    g_hist_count = 0;
    fp = fopen("/etc/passwd", "r");
    if (!fp) {
        g_hist_ok = 0;
        snprintf(g_hist_why, sizeof(g_hist_why), "无法读取 /etc/passwd");
        return;
    }

    while (fgets(line, sizeof(line), fp) && g_hist_count < CMDLOG_MAX_HIST) {
        char *fields[7];
        int   n = 0;
        char *p = line;
        const char *home;
        static const char *names[] = { ".bash_history", ".zsh_history" };
        int   k;

        while (n < 7) {
            char *colon = strchr(p, ':');

            fields[n++] = p;
            if (!colon)
                break;
            *colon = '\0';
            p = colon + 1;
        }
        if (n < 7)
            continue;
        fields[6][strcspn(fields[6], "\r\n")] = '\0';
        home = fields[5];
        if (home[0] != '/' || strcmp(home, "/") == 0)
            continue;

        for (k = 0; k < 2 && g_hist_count < CMDLOG_MAX_HIST; k++) {
            char        path[320];
            struct stat st;
            FILE       *probe;

            snprintf(path, sizeof(path), "%s/%s", home, names[k]);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            seen_files++;

            probe = fopen(path, "r");
            if (!probe) {
                unreadable++;
                continue;
            }
            fclose(probe);
            slh_strlcpy(g_hist[g_hist_count].path, path,
                        sizeof(g_hist[g_hist_count].path));
            g_hist[g_hist_count].off = 0;
            g_hist[g_hist_count].primed = 0;
            g_hist_count++;
        }
    }
    fclose(fp);

    g_hist_ok = g_hist_count > 0;
    if (g_hist_ok) {
        g_hist_why[0] = '\0';
    } else if (unreadable > 0) {
        /*
         * The common case on a single-user box: /root/.bash_history is 0600
         * root:root, so a service that deliberately does not run as root
         * cannot read it. Say so instead of pretending the feature works.
         */
        snprintf(g_hist_why, sizeof(g_hist_why),
                 "找到 %d 个历史文件但服务身份读不到（root 的 ~/.bash_history "
                 "通常是 0600 仅属主可读）", unreadable);
    } else {
        snprintf(g_hist_why, sizeof(g_hist_why),
                 "系统里没有 .bash_history / .zsh_history（已找到 %d 个）", seen_files);
    }
}

static int scan_history_locked(void)
{
    size_t i;
    size_t before = g_count;

    for (i = 0; i < g_hist_count; i++) {
        hist_file_t *h = &g_hist[i];
        struct stat  st;
        FILE        *fp;
        char         line[1024];

        if (stat(h->path, &st) != 0)
            continue;
        if (st.st_size < h->off)
            h->off = 0;                /* 被截断或轮转 */

        /*
         * On the very first sighting jump to the end: importing months of old
         * history would bury the events the user actually cares about.
         */
        if (!h->primed) {
            h->off = st.st_size;
            h->primed = 1;
            continue;
        }
        if (st.st_size == h->off)
            continue;

        fp = fopen(h->path, "r");
        if (!fp)
            continue;
        if (fseeko(fp, h->off, SEEK_SET) != 0) {
            fclose(fp);
            continue;
        }

        while (fgets(line, sizeof(line), fp)) {
            char *text = line;
            int   uid = -1;
            int   line_len = (int)strlen(line);

            h->off += line_len;

            /* zsh 会写 ": <开始时间>:<耗时>;<命令>" */
            if (text[0] == ':' && text[1] == ' ') {
                char *semi = strchr(text, ';');

                if (semi)
                    text = semi + 1;
            }
            text[strcspn(text, "\r\n")] = '\0';
            text = slh_trim(text);
            if (*text == '\0' || *text == '#')
                continue;

            /* 历史文件属于某个用户，用文件属主作为执行者 */
            if (uid < 0) {
                struct stat fs;

                if (stat(h->path, &fs) == 0)
                    uid = (int)fs.st_uid;
            }
            add_locked(slh_now_sec(), CMD_SOURCE_HISTORY, uid, 0, 0, text);
        }
        fclose(fp);
    }
    /* Offsets only change when something was actually read. */
    if (g_count != before)
        save_offsets_locked();
    return (int)(g_count - before);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int cmdlog_start(const config_t *cfg)
{
    snprintf(g_log_path, sizeof(g_log_path), "%s/commands.log", cfg->state_dir);
    snprintf(g_off_path, sizeof(g_off_path), "%s/cmdlog.offset", cfg->state_dir);

    pthread_mutex_lock(&g_lock);
    load_locked();
    discover_history_files_locked();
    load_offsets_locked();
    scan_procs_locked();               /* 第一轮只建立基线，不记录 */
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("commands: watching process list%s",
             g_hist_ok ? " and shell history" : "");
    if (!g_hist_ok)
        LOG_INFO("commands: shell history unavailable - %s", g_hist_why);
    return 0;
}

void cmdlog_stop(void)
{
    pthread_mutex_lock(&g_lock);
    save_locked();
    save_offsets_locked();
    pthread_mutex_unlock(&g_lock);
}

void cmdlog_poll(void)
{
    static int64_t last_hist;
    static int64_t last_discover;
    int            added;
    int64_t        now = slh_now_sec();

    pthread_mutex_lock(&g_lock);

    added = scan_procs_locked();

    /*
     * Re-discover the history files occasionally: a user's history file may
     * not exist at boot, and picking it up only at start-up would mean never
     * seeing it at all.
     */
    if (now - last_discover >= 60) {
        last_discover = now;
        discover_history_files_locked();
    }

    /* Reading history files is I/O; 10 s is plenty for interactive use. */
    if (now - last_hist >= 10) {
        last_hist = now;
        added += scan_history_locked();
    }

    /* Only touch the disk when something actually happened: this runs several
     * times a second, and rewriting the log each time was measurably the most
     * expensive thing the daemon did. */
    if (added > 0)
        append_locked(added);

    pthread_mutex_unlock(&g_lock);
}

size_t cmdlog_list(cmd_record_t *out, size_t max)
{
    size_t taken = 0;
    size_t i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_count && taken < max; i++) {
        size_t idx = (g_head + SLH_MAX_COMMANDS - 1 - i) % SLH_MAX_COMMANDS;

        out[taken++] = g_rec[idx];
    }
    pthread_mutex_unlock(&g_lock);
    return taken;
}

size_t cmdlog_list_deletes(cmd_record_t *out, size_t max)
{
    size_t taken = 0;
    size_t i;

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_count && taken < max; i++) {
        size_t idx = (g_head + SLH_MAX_COMMANDS - 1 - i) % SLH_MAX_COMMANDS;

        if (g_rec[idx].is_delete)
            out[taken++] = g_rec[idx];
    }
    pthread_mutex_unlock(&g_lock);
    return taken;
}

size_t cmdlog_delete_count(void)
{
    size_t n;

    pthread_mutex_lock(&g_lock);
    n = g_deletes;
    pthread_mutex_unlock(&g_lock);
    return n;
}

size_t cmdlog_total(void)
{
    size_t n;

    pthread_mutex_lock(&g_lock);
    n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t cmdlog_disk_bytes(void)
{
    uint64_t n;

    pthread_mutex_lock(&g_lock);
    n = g_disk_bytes;
    pthread_mutex_unlock(&g_lock);
    return n;
}

int cmdlog_history_status(char *why, size_t why_size)
{
    int ok;

    pthread_mutex_lock(&g_lock);
    ok = g_hist_ok;
    if (!ok && why && why_size)
        slh_strlcpy(why, g_hist_why, why_size);
    pthread_mutex_unlock(&g_lock);
    return ok ? 0 : -1;
}
