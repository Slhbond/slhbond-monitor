/*
 * gpu.c - GPU discovery and sampling for Mali parts under panfrost.
 */
#include "gpu.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "log.h"

#define DEVFREQ_SYSFS "/sys/class/devfreq"
#define DRI_DEV_DIR   "/dev/dri"
#define DEVICE_TREE   "/proc/device-tree"

/* ------------------------------------------------------------------ */
/* Previous power-on sample (for the active ratio)                     */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_prev_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_prev_active_ms;
static int64_t         g_prev_wall_ms;
static int             g_prev_valid;

/* ------------------------------------------------------------------ */
/* Model naming                                                        */
/* ------------------------------------------------------------------ */

/*
 * The device tree only says "arm,mali-bifrost" for the whole Bifrost family,
 * so the concrete marketing name has to come from the SoC. Keep this table
 * next to a comment explaining why it exists, so the next person knows the
 * string is a lookup and not a guess.
 */
typedef struct {
    const char *soc;      /* substring of the "vendor,soc" compatible */
    const char *gpu;
} soc_gpu_t;

static const soc_gpu_t g_soc_gpus[] = {
    { "rk3568", "Mali-G52 2EE" },
    { "rk3566", "Mali-G52 2EE" },
    { "rk3588", "Mali-G610 MC4" },
    { "rk3588s", "Mali-G610 MC4" },
    { "rk3399", "Mali-T860 MP4" },
    { "rk3328", "Mali-450 MP2" },
    { "rk3288", "Mali-T764" },
    { "rk3326", "Mali-G31 MP2" },
    { "rk3308", "Mali-G31" },
    { NULL, NULL }
};

/** Turn the compatible list into a human GPU name. */
static void derive_model(const char *compat, char *out, size_t out_size)
{
    const char *specific = NULL;
    int         i;

    /* 1. An explicit "arm,mali-gNN" style entry wins outright. */
    {
        const char *p = compat;

        while (*p) {
            size_t len = strlen(p);

            if (strncmp(p, "arm,mali-", 9) == 0) {
                const char *suffix = p + 9;

                if (strncmp(suffix, "bifrost", 7) != 0 &&
                    strncmp(suffix, "midgard", 7) != 0 &&
                    strncmp(suffix, "valhall", 7) != 0) {
                    specific = suffix;
                    break;
                }
            }
            p += len + 1;
        }
    }

    if (specific) {
        snprintf(out, out_size, "Mali-%.48s", specific);
        for (i = 0; out[i]; i++)
            out[i] = (char)toupper((unsigned char)out[i]);
        return;
    }

    /* 2. Otherwise map the SoC. */
    for (i = 0; g_soc_gpus[i].soc; i++) {
        if (strstr(compat, g_soc_gpus[i].soc)) {
            slh_strlcpy(out, g_soc_gpus[i].gpu, out_size);
            return;
        }
    }

    /* 3. Last resort: name the architecture family honestly. */
    if (strstr(compat, "mali-bifrost"))
        slh_strlcpy(out, "Mali (Bifrost)", out_size);
    else if (strstr(compat, "mali-midgard"))
        slh_strlcpy(out, "Mali (Midgard)", out_size);
    else if (strstr(compat, "mali-valhall"))
        slh_strlcpy(out, "Mali (Valhall)", out_size);
    else
        slh_strlcpy(out, "GPU", out_size);
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

static int looks_like_gpu(const char *name)
{
    return strstr(name, "gpu") != NULL;
}

/** Locate the GPU's devfreq node; returns 1 and fills `node` on success. */
static int find_devfreq(char *node, size_t node_size)
{
    DIR           *dir;
    struct dirent *entry;
    int            found = 0;

    dir = opendir(DEVFREQ_SYSFS);
    if (!dir)
        return 0;

    while ((entry = readdir(dir)) != NULL) {
        char    dev_link[512];
        char    target[512];
        ssize_t n;

        if (entry->d_name[0] == '.')
            continue;

        /* Resolve the backing device so a stray "gpu" in an unrelated
         * devfreq name cannot win, and vice versa. */
        snprintf(dev_link, sizeof(dev_link), "%s/%s/device", DEVFREQ_SYSFS,
                 entry->d_name);
        n = readlink(dev_link, target, sizeof(target) - 1);
        if (n <= 0)
            continue;
        target[n] = '\0';

        if (!looks_like_gpu(target) && !looks_like_gpu(entry->d_name))
            continue;

        snprintf(node, node_size, "%s/%s", DEVFREQ_SYSFS, entry->d_name);
        found = 1;
        break;
    }
    closedir(dir);
    return found;
}

/** Read the device-tree compatible list of the GPU node. */
static void read_compatibles(const char *devfreq_node, char *out, size_t out_size)
{
    char path[512];
    char entry[160];
    int  i;

    out[0] = '\0';

    /* Preferred: the devfreq device's own of_node. */
    snprintf(path, sizeof(path), "%.240s/device/of_node/compatible", devfreq_node);
    for (i = 0; i < 4; i++) {
        if (!slh_devicetree_string(path, i, entry, sizeof(entry)))
            break;
        slh_strlcat(out, entry, out_size);
        slh_strlcat(out, " ", out_size);
    }
    if (out[0])
        return;

    /* Fallback: scan /proc/device-tree for a gpu node. */
    {
        DIR           *dir = opendir(DEVICE_TREE);
        struct dirent *de;

        if (!dir)
            return;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "gpu", 3) != 0 &&
                strstr(de->d_name, "gpu") == NULL)
                continue;
            snprintf(path, sizeof(path), "%s/%s/compatible", DEVICE_TREE,
                     de->d_name);
            for (i = 0; i < 4; i++) {
                if (!slh_devicetree_string(path, i, entry, sizeof(entry)))
                    break;
                slh_strlcat(out, entry, out_size);
                slh_strlcat(out, " ", out_size);
            }
            if (out[0])
                break;
        }
        closedir(dir);
    }
}

/** Read the DRI render node belonging to the GPU. */
static void find_render_node(const char *devfreq_node, char *out, size_t out_size)
{
    char path[512];
    char card[320];
    char target[512];
    ssize_t n;

    out[0] = '\0';

    snprintf(path, sizeof(path), "%.240s/device/drm", devfreq_node);
    {
        DIR           *dir = opendir(path);
        struct dirent *de;

        if (!dir)
            return;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "renderD", 7) != 0)
                continue;
            snprintf(card, sizeof(card), "%.240s/%.12s", path, de->d_name);
            n = readlink(card, target, sizeof(target) - 1);
            (void)n;
            snprintf(out, out_size, "%.40s/%.15s", DRI_DEV_DIR, de->d_name);
            break;
        }
        closedir(dir);
    }
}

/* ------------------------------------------------------------------ */
/* Frequency table                                                     */
/* ------------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

static void read_frequencies(gpu_t *g, const char *node)
{
    char  path[512];
    char  buffer[512];
    char *p;

    snprintf(path, sizeof(path), "%.240s/available_frequencies", node);
    if (slh_read_text(path, buffer, sizeof(buffer))) {
        p = buffer;
        while (*p && g->freq_step_count < SLH_MAX_GPU_FREQ_STEPS) {
            char *end = NULL;
            unsigned long long v = strtoull(p, &end, 10);

            if (end == p)
                break;
            g->freq_steps[g->freq_step_count++] = (uint64_t)v;
            p = end;
            while (*p == ' ' || *p == '\t')
                p++;
        }
        if (g->freq_step_count > 1)
            qsort(g->freq_steps, g->freq_step_count, sizeof(g->freq_steps[0]),
                  cmp_u64);
    }

    if (g->freq_step_count == 0) {
        /* Derive a usable range from min/max when the list is unavailable. */
        if (g->freq_min_hz)
            g->freq_steps[g->freq_step_count++] = g->freq_min_hz;
        if (g->freq_max_hz && g->freq_max_hz != g->freq_min_hz)
            g->freq_steps[g->freq_step_count++] = g->freq_max_hz;
    }

    g->freq_step_index = -1;
    {
        size_t i;
        for (i = 0; i < g->freq_step_count; i++) {
            if (g->freq_steps[i] == g->freq_cur_hz) {
                g->freq_step_index = (int)i;
                break;
            }
        }
        /* Exact match failed: use the nearest step at or below. */
        if (g->freq_step_index < 0 && g->freq_step_count > 0) {
            for (i = 0; i < g->freq_step_count; i++) {
                if (g->freq_steps[i] <= g->freq_cur_hz)
                    g->freq_step_index = (int)i;
            }
            if (g->freq_step_index < 0)
                g->freq_step_index = 0;
        }
    }
}

/**
 * Average operating frequency over the device's lifetime, computed from the
 * devfreq transition table. Rows look like:
 *
 *        From  :   To
 *              : 200000000 300000000 ...   time(ms)
 *   * 600000000:         0         0 ...        13
 *
 * Only rows that start (after optional '*' and spaces) with a number followed
 * by ':' are data rows; everything else is header noise.
 */
static uint64_t read_avg_frequency(const char *node)
{
    char   path[512];
    FILE  *fp;
    char   line[512];
    double weighted = 0.0;
    double total_ms = 0.0;

    snprintf(path, sizeof(path), "%.240s/trans_stat", node);
    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp)) {
        char              *p = line;
        char              *end = NULL;
        unsigned long long freq;
        double             last = 0.0;
        char              *q;

        while (*p == ' ' || *p == '\t' || *p == '*')
            p++;
        if (!isdigit((unsigned char)*p))
            continue;

        freq = strtoull(p, &end, 10);
        q = end;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != ':')
            continue;
        q++;

        /* The last numeric column is the accumulated time in ms. */
        while (*q) {
            char  *next = NULL;
            double value;

            while (*q == ' ' || *q == '\t')
                q++;
            if (!*q)
                break;
            value = strtod(q, &next);
            if (next == q)
                break;
            last = value;
            q = next;
        }

        weighted += (double)freq * last;
        total_ms += last;
    }
    fclose(fp);

    if (total_ms <= 0.0)
        return 0;
    return (uint64_t)(weighted / total_ms);
}

/* ------------------------------------------------------------------ */
/* Load counter probe                                                  */
/* ------------------------------------------------------------------ */

/*
 * panfrost exports no utilisation counter. The vendor mali kbase driver and
 * some out-of-tree Bifrost drivers do, in several different places, so probe
 * the known ones and report honestly when none is present.
 */
static double find_load_counter(const char *node, char *source, size_t source_size)
{
    static const char *const suffixes[] = {
        "load",            /* devfreq load, present on some vendor kernels */
        "gpu_load",
        "device/gpu_load",
        NULL
    };
    char path[512];
    char buffer[128];
    int  i;

    for (i = 0; suffixes[i]; i++) {
        char  *end = NULL;
        double value;

        /* Built with an explicit format rather than a format string from the
         * table, so -Wformat-nonliteral stays meaningful elsewhere. */
        snprintf(path, sizeof(path), "%.240s/%.15s", node, suffixes[i]);
        if (!slh_read_text(path, buffer, sizeof(buffer)))
            continue;
        value = strtod(buffer, &end);
        if (end == buffer)
            continue;
        if (value < 0.0 || value > 100.0)
            continue;               /* a counter, not a percentage */
        slh_strlcpy(source, path, source_size);
        return value;
    }
    return -1.0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void gpu_collect(gpu_t *g)
{
    char node[512];
    char path[640];
    char text[192];

    memset(g, 0, sizeof(*g));
    g->load_percent = -1.0;
    g->temp_c = -1.0;
    g->active_ratio_percent = -1.0;
    g->freq_step_index = -1;

    if (!find_devfreq(node, sizeof(node))) {
        /* No devfreq node; still report the render node when one exists. */
        snprintf(path, sizeof(path), "%s/renderD128", DRI_DEV_DIR);
        if (access(path, F_OK) == 0) {
            g->present = 1;
            slh_strlcpy(g->render_node, path, sizeof(g->render_node));
            slh_strlcpy(g->model, "GPU", sizeof(g->model));
        }
        return;
    }

    g->present = 1;
    slh_strlcpy(g->devfreq_name, node, sizeof(g->devfreq_name));

    /* --- identity -------------------------------------------------- */
    read_compatibles(node, g->compatible, sizeof(g->compatible));
    if (g->compatible[0])
        derive_model(g->compatible, g->model, sizeof(g->model));
    else
        slh_strlcpy(g->model, "GPU", sizeof(g->model));

    snprintf(path, sizeof(path), "%s/device/driver", node);
    if (!slh_readlink_basename(path, g->driver, sizeof(g->driver))) {
        snprintf(path, sizeof(path), "%s/device/driver", node);
        slh_strlcpy(g->driver, "unknown", sizeof(g->driver));
    }

    find_render_node(node, g->render_node, sizeof(g->render_node));

    /* --- frequency ------------------------------------------------- */
    snprintf(path, sizeof(path), "%s/cur_freq", node);
    g->freq_cur_hz = slh_read_u64(path, 0);
    snprintf(path, sizeof(path), "%s/min_freq", node);
    g->freq_min_hz = slh_read_u64(path, 0);
    snprintf(path, sizeof(path), "%s/max_freq", node);
    g->freq_max_hz = slh_read_u64(path, 0);
    snprintf(path, sizeof(path), "%s/governor", node);
    if (slh_read_text(path, text, sizeof(text)))
        slh_strlcpy(g->governor, text, sizeof(g->governor));

    read_frequencies(g, node);
    g->freq_avg_hz = read_avg_frequency(node);

    /* --- power / activity ------------------------------------------ */
    snprintf(path, sizeof(path), "%s/device/power/runtime_status", node);
    if (slh_read_text(path, text, sizeof(text)))
        slh_strlcpy(g->power_state, text, sizeof(g->power_state));

    snprintf(path, sizeof(path), "%s/device/power/runtime_active_time", node);
    g->active_ms = slh_read_u64(path, 0);

    {
        int64_t now = slh_monotonic_ms();

        pthread_mutex_lock(&g_prev_lock);
        if (g_prev_valid) {
            int64_t  wall_delta = now - g_prev_wall_ms;
            uint64_t active_delta = g->active_ms > g_prev_active_ms
                                        ? g->active_ms - g_prev_active_ms : 0;

            if (wall_delta > 0) {
                double ratio = (double)active_delta * 100.0 / (double)wall_delta;
                g->active_ratio_percent = ratio > 100.0 ? 100.0 : ratio;
            }
        }
        g_prev_active_ms = g->active_ms;
        g_prev_wall_ms = now;
        g_prev_valid = 1;
        pthread_mutex_unlock(&g_prev_lock);
    }

    /* --- thermal --------------------------------------------------- */
    {
        DIR           *dir = opendir("/sys/class/hwmon");
        struct dirent *de;

        if (dir) {
            while ((de = readdir(dir)) != NULL) {
                char name[64];

                if (strncmp(de->d_name, "hwmon", 5) != 0)
                    continue;
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name",
                         de->d_name);
                if (!slh_read_text(path, name, sizeof(name)))
                    continue;
                if (strstr(name, "gpu") == NULL)
                    continue;

                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input",
                         de->d_name);
                {
                    int64_t milli = slh_read_i64(path, -1);

                    if (milli > 0) {
                        g->temp_c = (double)milli / 1000.0;
                        slh_strlcpy(g->temp_label, name, sizeof(g->temp_label));
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }

    /* --- load counter ---------------------------------------------- */
    g->load_percent = find_load_counter(node, g->load_source,
                                        sizeof(g->load_source));

    LOG_DEBUG("gpu: %s driver=%s freq=%llu/%llu governor=%s state=%s active=%llums",
              g->model, g->driver, (unsigned long long)g->freq_cur_hz,
              (unsigned long long)g->freq_max_hz, g->governor, g->power_state,
              (unsigned long long)g->active_ms);
}

void gpu_to_json(strbuf_t *sb, const gpu_t *g, int *first)
{
    size_t i;

    json_add_bool(sb, "present", g->present, first);
    json_add_str(sb, "model", g->model[0] ? g->model : NULL, first);
    json_add_str(sb, "compatible", g->compatible[0] ? g->compatible : NULL, first);
    json_add_str(sb, "driver", g->driver[0] ? g->driver : NULL, first);
    json_add_str(sb, "render_node",
                 g->render_node[0] ? g->render_node : NULL, first);

    json_add_uint(sb, "freq_cur_hz", g->freq_cur_hz, first);
    json_add_uint(sb, "freq_min_hz", g->freq_min_hz, first);
    json_add_uint(sb, "freq_max_hz", g->freq_max_hz, first);
    json_add_uint(sb, "freq_avg_hz", g->freq_avg_hz, first);
    json_add_int(sb, "freq_step_index", g->freq_step_index, first);
    json_add_str(sb, "governor", g->governor[0] ? g->governor : NULL, first);
    json_add_str(sb, "power_state",
                 g->power_state[0] ? g->power_state : NULL, first);

    json_key(sb, "freq_steps_hz", first);
    sb_appendc(sb, '[');
    for (i = 0; i < g->freq_step_count; i++) {
        if (i)
            sb_appendc(sb, ',');
        sb_appendf(sb, "%llu", (unsigned long long)g->freq_steps[i]);
    }
    sb_appendc(sb, ']');

    if (g->temp_c < 0.0)
        json_add_null(sb, "temp_c", first);
    else
        json_add_double(sb, "temp_c", g->temp_c, first);
    json_add_str(sb, "temp_label", g->temp_label[0] ? g->temp_label : NULL, first);

    if (g->load_percent < 0.0)
        json_add_null(sb, "load_percent", first);
    else
        json_add_double(sb, "load_percent", g->load_percent, first);
    json_add_str(sb, "load_source",
                 g->load_source[0] ? g->load_source : NULL, first);

    if (g->active_ratio_percent < 0.0)
        json_add_null(sb, "active_ratio_percent", first);
    else
        json_add_double(sb, "active_ratio_percent", g->active_ratio_percent, first);
    json_add_uint(sb, "active_ms", g->active_ms, first);
}
