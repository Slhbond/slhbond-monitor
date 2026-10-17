/*
 * cpustat.c - detailed CPU view.
 *
 * Everything here is read from standard kernel interfaces:
 *   /proc/stat                 per-CPU cumulative jiffies  -> load deltas
 *   /proc/cpuinfo              model name, CPU part
 *   /proc/device-tree/...      SoC compatible + board model
 *   /sys/devices/system/cpu/   topology and cpufreq
 *   /sys/class/thermal/        temperatures (millidegrees)
 */
#include "cpustat.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"
#include "log.h"

#define PROC_STAT     "/proc/stat"
#define PROC_CPUINFO  "/proc/cpuinfo"
#define DT_COMPATIBLE "/proc/device-tree/compatible"
#define DT_MODEL      "/proc/device-tree/model"
#define CPU_SYSFS     "/sys/devices/system/cpu"
#define CPUFREQ_SYSFS "/sys/devices/system/cpu/cpufreq"
#define THERMAL_SYSFS "/sys/class/thermal"

/* ------------------------------------------------------------------ */
/* Small sysfs/proc helpers                                            */
/* ------------------------------------------------------------------ */

/** Read a text file, stripping the trailing newline. Returns 1 on success. */
static int read_text(const char *path, char *out, size_t out_size)
{
    FILE *fp;
    size_t len;

    if (out_size == 0)
        return 0;
    out[0] = '\0';
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        out[0] = '\0';
        return 0;
    }
    fclose(fp);
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                       out[len - 1] == '\0'))
        out[--len] = '\0';
    return out[0] != '\0';
}

static uint64_t read_u64(const char *path, uint64_t fallback)
{
    char          buf[64];
    char         *end = NULL;
    unsigned long long value;

    if (!read_text(path, buf, sizeof(buf)))
        return fallback;
    value = strtoull(buf, &end, 10);
    if (end == buf)
        return fallback;
    return (uint64_t)value;
}

/* ------------------------------------------------------------------ */
/* Previous /proc/stat sample                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t total;
    uint64_t idle;
} cpu_sample_t;

/*
 * /proc/stat counters are cumulative, so a percentage needs two readings.
 * The previous sample is cached here behind one mutex; the first call after
 * start-up therefore reports -1.0 ("not yet sampled").
 */
static pthread_mutex_t g_sample_lock = PTHREAD_MUTEX_INITIALIZER;
static cpu_sample_t    g_prev_agg;
static cpu_sample_t    g_prev_core[SLH_MAX_CPUS];
static int             g_prev_valid;

static double percent_between(const cpu_sample_t *prev, const cpu_sample_t *now)
{
    uint64_t d_total;
    uint64_t d_idle;
    double   busy;

    if (now->total <= prev->total)
        return -1.0;
    d_total = now->total - prev->total;
    d_idle = now->idle > prev->idle ? now->idle - prev->idle : 0;
    if (d_idle > d_total)
        d_idle = d_total;
    busy = (double)(d_total - d_idle) * 100.0 / (double)d_total;
    if (busy < 0.0)
        busy = 0.0;
    if (busy > 100.0)
        busy = 100.0;
    return busy;
}

/**
 * Parse one "cpu" or "cpuN" line of /proc/stat.
 * Returns 1 on success and fills `index` (-1 for the aggregate line).
 */
static int parse_stat_line(const char *line, int *index, cpu_sample_t *out)
{
    const char             *p = line + 3;   /* past "cpu" */
    unsigned long long      f[10];
    uint64_t                total = 0;
    int                     n;
    int                     i;

    if (strncmp(line, "cpu", 3) != 0)
        return 0;

    if (isdigit((unsigned char)*p)) {
        *index = atoi(p);
        while (isdigit((unsigned char)*p))
            p++;
    } else if (*p == ' ' || *p == '\t') {
        *index = -1;                        /* the aggregate "cpu " line */
    } else {
        return 0;                           /* e.g. "ctxt", "btime" */
    }

    memset(f, 0, sizeof(f));
    n = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &f[0], &f[1], &f[2], &f[3], &f[4],
               &f[5], &f[6], &f[7], &f[8], &f[9]);
    if (n < 4)
        return 0;

    for (i = 0; i < n; i++)
        total += (uint64_t)f[i];

    out->total = total;
    out->idle = (uint64_t)f[3] + (uint64_t)f[4];   /* idle + iowait */
    return 1;
}

static void read_load(cpustat_t *cs)
{
    FILE          *fp;
    char           line[512];
    cpu_sample_t   now_agg;
    cpu_sample_t   now_core[SLH_MAX_CPUS];
    int            have_agg = 0;
    int            have_core[SLH_MAX_CPUS];
    size_t         i;

    memset(&now_agg, 0, sizeof(now_agg));
    memset(now_core, 0, sizeof(now_core));
    memset(have_core, 0, sizeof(have_core));

    fp = fopen(PROC_STAT, "r");
    if (!fp) {
        LOG_WARN("cannot read %s: %s", PROC_STAT, strerror(errno));
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        int          index = -1;
        cpu_sample_t sample;

        if (strncmp(line, "cpu", 3) != 0)
            continue;
        if (!parse_stat_line(line, &index, &sample))
            continue;

        if (index < 0) {
            now_agg = sample;
            have_agg = 1;
        } else if (index < SLH_MAX_CPUS) {
            now_core[index] = sample;
            have_core[index] = 1;
            if ((size_t)index + 1 > cs->core_count)
                cs->core_count = (size_t)index + 1;
        }
    }
    fclose(fp);

    pthread_mutex_lock(&g_sample_lock);

    if (g_prev_valid && have_agg)
        cs->usage_percent = percent_between(&g_prev_agg, &now_agg);

    cs->max_core_percent = -1.0;
    cs->busiest_core = -1;

    for (i = 0; i < cs->core_count; i++) {
        cpu_sample_t *prev = &g_prev_core[i];

        cs->core[i].index = (int)i;
        cs->core[i].usage_percent = -1.0;

        if (!have_core[i])
            continue;
        if (g_prev_valid) {
            double pct = percent_between(prev, &now_core[i]);

            cs->core[i].usage_percent = pct;
            if (pct > cs->max_core_percent) {
                cs->max_core_percent = pct;
                cs->busiest_core = (int)i;
            }
        }
        g_prev_core[i] = now_core[i];
    }

    if (have_agg)
        g_prev_agg = now_agg;
    g_prev_valid = 1;

    pthread_mutex_unlock(&g_sample_lock);

    /* When the aggregate line was missing, fall back to the per-core mean. */
    if (cs->usage_percent < 0.0 && cs->core_count > 0) {
        double sum = 0.0;
        int    counted = 0;

        for (i = 0; i < cs->core_count; i++) {
            if (cs->core[i].usage_percent >= 0.0) {
                sum += cs->core[i].usage_percent;
                counted++;
            }
        }
        if (counted)
            cs->usage_percent = sum / (double)counted;
    }
}

/* ------------------------------------------------------------------ */
/* Identity: CPU model, SoC and core name                              */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned    part;
    const char *name;
} arm_part_t;

/* ARM CPU part numbers we are likely to meet on SBCs. */
static const arm_part_t g_arm_parts[] = {
    { 0xd03, "Cortex-A53" }, { 0xd04, "Cortex-A35" }, { 0xd05, "Cortex-A55" },
    { 0xd07, "Cortex-A57" }, { 0xd08, "Cortex-A72" }, { 0xd09, "Cortex-A73" },
    { 0xd0a, "Cortex-A75" }, { 0xd0b, "Cortex-A76" }, { 0xd0c, "Neoverse-N1" },
    { 0xd0d, "Cortex-A77" }, { 0xd40, "Neoverse-V1" }, { 0xd41, "Cortex-A78" },
    { 0xd44, "Cortex-X1" }, { 0xd46, "Cortex-A510" }, { 0xd47, "Cortex-A710" },
    { 0xd48, "Cortex-X2" }, { 0xd49, "Neoverse-N2" }, { 0xd4d, "Cortex-A715" },
    { 0xd4e, "Cortex-X3" }, { 0xd81, "Cortex-A720" }, { 0xd82, "Cortex-X4" },
    { 0xd85, "Cortex-X925" },
    { 0, NULL }
};

/** Look up "model name"/"Hardware" and "CPU part" in /proc/cpuinfo. */
static void read_cpuinfo(cpustat_t *cs)
{
    FILE *fp;
    char  line[512];
    int   have_model = 0;
    int   part_seen = 0;
    unsigned part = 0;

    fp = fopen(PROC_CPUINFO, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        char *key;
        char *value;

        if (!colon)
            continue;
        *colon = '\0';
        key = slh_trim(line);
        value = slh_trim(colon + 1);

        if (!have_model &&
            (slh_strcasecmp(key, "model name") == 0 ||
             slh_strcasecmp(key, "Hardware") == 0 ||
             slh_strcasecmp(key, "cpu model") == 0)) {
            slh_strlcpy(cs->model, value, sizeof(cs->model));
            have_model = 1;
        } else if (!part_seen && slh_strcasecmp(key, "CPU part") == 0) {
            part = (unsigned)strtoul(value, NULL, 0);
            part_seen = 1;
        }
        if (have_model && part_seen)
            break;
    }
    fclose(fp);

    if (part_seen) {
        int i;
        for (i = 0; g_arm_parts[i].name; i++) {
            if (g_arm_parts[i].part == part) {
                slh_strlcpy(cs->core_name, g_arm_parts[i].name,
                            sizeof(cs->core_name));
                break;
            }
        }
        if (cs->core_name[0] == '\0')
            snprintf(cs->core_name, sizeof(cs->core_name), "ARM 0x%03x", part);
    }
}

/** "rockchip,rk3568" -> "Rockchip RK3568". */
static void format_compatible(const char *entry, char *out, size_t out_size)
{
    const char *comma = strchr(entry, ',');
    char        vendor[48];
    char        chip[64];
    size_t      i;

    if (!comma) {
        slh_strlcpy(out, entry, out_size);
        return;
    }
    slh_strlcpy(vendor, entry, (size_t)(comma - entry) + 1);
    slh_strlcpy(chip, comma + 1, sizeof(chip));

    if (vendor[0])
        vendor[0] = (char)toupper((unsigned char)vendor[0]);
    for (i = 0; chip[i]; i++)
        chip[i] = (char)toupper((unsigned char)chip[i]);

    snprintf(out, out_size, "%.31s %.63s", vendor, chip);
}

static int is_soc_vendor(const char *vendor, size_t len)
{
    static const char *vendors[] = {
        "rockchip", "allwinner", "amlogic", "nxp", "ti", "qcom", "mediatek",
        "broadcom", "samsung", "st", "marvell", "nvidia", "apple", "intel",
        "amd", "loongson", "phytium", NULL
    };
    int i;

    for (i = 0; vendors[i]; i++) {
        if (strlen(vendors[i]) == len && strncmp(vendor, vendors[i], len) == 0)
            return 1;
    }
    return 0;
}

/** Read the device tree for the SoC compatible string and board model. */
static void read_device_tree(cpustat_t *cs)
{
    char  buffer[1024];
    FILE *fp;
    size_t len;
    size_t offset;
    char  best[128];
    char  last[128];

    best[0] = '\0';
    last[0] = '\0';

    fp = fopen(DT_COMPATIBLE, "rb");
    if (fp) {
        len = fread(buffer, 1, sizeof(buffer), fp);
        fclose(fp);

        /* The property is a list of NUL-terminated strings. */
        for (offset = 0; offset < len; ) {
            const char *entry = buffer + offset;
            size_t      entry_len = strnlen(entry, len - offset);
            const char *comma;

            if (entry_len == 0)
                break;
            if (entry_len < sizeof(last))
                slh_strlcpy(last, entry, sizeof(last));

            comma = memchr(entry, ',', entry_len);
            if (comma && is_soc_vendor(entry, (size_t)(comma - entry))) {
                slh_strlcpy(best, entry, sizeof(best));
                break;
            }
            offset += entry_len + 1;
        }
        if (best[0] == '\0' && last[0] != '\0')
            slh_strlcpy(best, last, sizeof(best));
    }

    if (best[0] != '\0')
        format_compatible(best, cs->soc, sizeof(cs->soc));

    /* The board model is a NUL-terminated string too. */
    fp = fopen(DT_MODEL, "rb");
    if (fp) {
        len = fread(buffer, 1, sizeof(buffer) - 1, fp);
        fclose(fp);
        buffer[len] = '\0';
        if (len > 0)
            slh_strlcpy(cs->machine, buffer, sizeof(cs->machine));
    }
}

/** Count physical cores as unique (package, core_id) pairs. */
static void read_topology(cpustat_t *cs)
{
    DIR           *dir;
    struct dirent *entry;
    int            threads = 0;
    char           seen[SLH_MAX_CPUS][48];
    int            seen_count = 0;
    int            i;

    dir = opendir(CPU_SYSFS);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            char  topo[300];
            char  pkg[32];
            char  core[32];
            char  key[80];
            int   duplicate = 0;
            int   index;

            if (strncmp(entry->d_name, "cpu", 3) != 0 ||
                !isdigit((unsigned char)entry->d_name[3]))
                continue;
            index = atoi(entry->d_name + 3);
            if (index < 0 || index >= SLH_MAX_CPUS)
                continue;
            threads++;

            snprintf(topo, sizeof(topo), "%s/%s/topology", CPU_SYSFS, entry->d_name);
            {
                char path[340];
                snprintf(path, sizeof(path), "%s/physical_package_id", topo);
                if (!read_text(path, pkg, sizeof(pkg)))
                    slh_strlcpy(pkg, "0", sizeof(pkg));
                snprintf(path, sizeof(path), "%s/core_id", topo);
                if (!read_text(path, core, sizeof(core)))
                    snprintf(core, sizeof(core), "%d", index);
            }
            snprintf(key, sizeof(key), "%s:%s", pkg, core);

            for (i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], key) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate && seen_count < SLH_MAX_CPUS)
                slh_strlcpy(seen[seen_count++], key, sizeof(seen[0]));
        }
        closedir(dir);
    }

    cs->threads = threads > 0 ? threads : (int)cs->core_count;
    cs->cores = seen_count > 0 ? seen_count : cs->threads;
    if (cs->threads == 0)
        cs->threads = 1;
    if (cs->cores == 0)
        cs->cores = 1;
}

/* ------------------------------------------------------------------ */
/* cpufreq                                                             */
/* ------------------------------------------------------------------ */

static void read_freq(cpustat_t *cs)
{
    char  path[320];
    char  text[64];
    DIR  *dir;
    struct dirent *entry;
    size_t i;

    for (i = 0; i < cs->core_count; i++) {
        uint64_t khz;

        snprintf(path, sizeof(path), "%s/cpu%zu/cpufreq/scaling_cur_freq",
                 CPU_SYSFS, i);
        khz = read_u64(path, 0);
        cs->core[i].freq_khz = khz;
        if (khz > cs->freq_cur_khz)
            cs->freq_cur_khz = khz;
    }

    /* policy0 carries the shared limits on every modern kernel. */
    snprintf(path, sizeof(path), "%s/policy0/scaling_min_freq", CPUFREQ_SYSFS);
    cs->freq_min_khz = read_u64(path, 0);
    snprintf(path, sizeof(path), "%s/policy0/scaling_max_freq", CPUFREQ_SYSFS);
    cs->freq_max_khz = read_u64(path, 0);
    snprintf(path, sizeof(path), "%s/policy0/scaling_governor", CPUFREQ_SYSFS);
    if (read_text(path, text, sizeof(text)))
        slh_strlcpy(cs->governor, text, sizeof(cs->governor));

    /* Fall back to any policy directory when policy0 does not exist. */
    if (cs->freq_max_khz == 0) {
        dir = opendir(CPUFREQ_SYSFS);
        if (dir) {
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, "policy", 6) != 0)
                    continue;
                snprintf(path, sizeof(path), "%s/%s/scaling_max_freq",
                         CPUFREQ_SYSFS, entry->d_name);
                cs->freq_max_khz = read_u64(path, cs->freq_max_khz);
                snprintf(path, sizeof(path), "%s/%s/scaling_min_freq",
                         CPUFREQ_SYSFS, entry->d_name);
                if (cs->freq_min_khz == 0)
                    cs->freq_min_khz = read_u64(path, 0);
                snprintf(path, sizeof(path), "%s/%s/scaling_governor",
                         CPUFREQ_SYSFS, entry->d_name);
                if (cs->governor[0] == '\0')
                    read_text(path, cs->governor, sizeof(cs->governor));
            }
            closedir(dir);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Thermal                                                             */
/* ------------------------------------------------------------------ */

/** 1 when the zone name looks like it measures the CPU/SoC. */
static int zone_is_cpu(const char *type)
{
    static const char *needles[] = { "cpu", "soc", "core", "cluster", "package", NULL };
    size_t i;
    int    n;

    for (n = 0; needles[n]; n++) {
        size_t len = strlen(needles[n]);

        for (i = 0; type[i]; i++) {
            size_t k;
            for (k = 0; k < len; k++) {
                if (tolower((unsigned char)type[i + k]) != needles[n][k])
                    break;
            }
            if (k == len)
                return 1;
        }
    }
    return 0;
}

static void read_thermal(cpustat_t *cs)
{
    DIR           *dir;
    struct dirent *entry;
    double         best_cpu = -1.0;
    double         best_any = -1.0;
    char           label_cpu[64];
    char           label_any[64];

    label_cpu[0] = '\0';
    label_any[0] = '\0';

    dir = opendir(THERMAL_SYSFS);
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        char   path[340];
        char   type[64];
        double celsius;

        if (strncmp(entry->d_name, "thermal_zone", 12) != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s/type", THERMAL_SYSFS, entry->d_name);
        if (!read_text(path, type, sizeof(type)))
            slh_strlcpy(type, entry->d_name, sizeof(type));

        snprintf(path, sizeof(path), "%s/%s/temp", THERMAL_SYSFS, entry->d_name);
        {
            uint64_t milli = read_u64(path, UINT64_MAX);

            if (milli == UINT64_MAX)
                continue;
            celsius = (double)milli / 1000.0;
        }

        if (celsius > best_any) {
            best_any = celsius;
            slh_strlcpy(label_any, type, sizeof(label_any));
        }
        if (zone_is_cpu(type) && celsius > best_cpu) {
            best_cpu = celsius;
            slh_strlcpy(label_cpu, type, sizeof(label_cpu));
        }
    }
    closedir(dir);

    /* Prefer a CPU-specific zone; otherwise fall back to the hottest one. */
    if (best_cpu >= 0.0) {
        cs->temp_c = best_cpu;
        slh_strlcpy(cs->temp_label, label_cpu, sizeof(cs->temp_label));
    } else if (best_any >= 0.0) {
        cs->temp_c = best_any;
        slh_strlcpy(cs->temp_label, label_any, sizeof(cs->temp_label));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void cpustat_collect(cpustat_t *cs)
{
    size_t i;

    memset(cs, 0, sizeof(*cs));
    cs->usage_percent = -1.0;
    cs->max_core_percent = -1.0;
    cs->busiest_core = -1;
    cs->temp_c = -1.0;
    for (i = 0; i < SLH_MAX_CPUS; i++)
        cs->core[i].usage_percent = -1.0;

    read_cpuinfo(cs);
    read_device_tree(cs);
    read_load(cs);          /* fills core_count, per-core and average load */
    read_topology(cs);
    read_freq(cs);
    read_thermal(cs);
}

void cpustat_to_json(strbuf_t *sb, const cpustat_t *cs, int *first)
{
    size_t i;

    json_add_str(sb, "model", cs->model[0] ? cs->model : NULL, first);
    json_add_str(sb, "soc", cs->soc[0] ? cs->soc : NULL, first);
    json_add_str(sb, "machine", cs->machine[0] ? cs->machine : NULL, first);
    json_add_str(sb, "core_name", cs->core_name[0] ? cs->core_name : NULL, first);
    json_add_int(sb, "cores", cs->cores, first);
    json_add_int(sb, "threads", cs->threads, first);

    if (cs->usage_percent < 0.0)
        json_add_null(sb, "usage_percent", first);
    else
        json_add_double(sb, "usage_percent", cs->usage_percent, first);

    if (cs->max_core_percent < 0.0)
        json_add_null(sb, "max_core_percent", first);
    else
        json_add_double(sb, "max_core_percent", cs->max_core_percent, first);
    json_add_int(sb, "busiest_core", cs->busiest_core, first);

    json_add_uint(sb, "freq_cur_khz", cs->freq_cur_khz, first);
    json_add_uint(sb, "freq_min_khz", cs->freq_min_khz, first);
    json_add_uint(sb, "freq_max_khz", cs->freq_max_khz, first);
    json_add_str(sb, "governor", cs->governor[0] ? cs->governor : NULL, first);

    if (cs->temp_c < 0.0)
        json_add_null(sb, "temp_c", first);
    else
        json_add_double(sb, "temp_c", cs->temp_c, first);
    json_add_str(sb, "temp_label", cs->temp_label[0] ? cs->temp_label : NULL, first);

    /* Per-core arrays — this is what the bar chart renders. */
    json_key(sb, "core_usage", first);
    sb_appendc(sb, '[');
    for (i = 0; i < cs->core_count; i++) {
        if (i)
            sb_appendc(sb, ',');
        if (cs->core[i].usage_percent < 0.0)
            sb_append(sb, "null");
        else
            sb_appendf(sb, "%.1f", cs->core[i].usage_percent);
    }
    sb_appendc(sb, ']');

    json_key(sb, "core_freq_khz", first);
    sb_appendc(sb, '[');
    for (i = 0; i < cs->core_count; i++) {
        if (i)
            sb_appendc(sb, ',');
        sb_appendf(sb, "%llu", (unsigned long long)cs->core[i].freq_khz);
    }
    sb_appendc(sb, ']');

    json_add_uint(sb, "core_count", (unsigned long long)cs->core_count, first);
}
