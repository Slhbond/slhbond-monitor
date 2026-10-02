/*
 * npu.c - NPU detection and load sampling.
 */
#include "npu.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"
#include "log.h"

#define RKNPU_DEBUGFS "/sys/kernel/debug/rknpu"
#define DEVFREQ_SYSFS "/sys/class/devfreq"
#define DEVICE_TREE   "/proc/device-tree"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int read_text(const char *path, char *out, size_t out_size)
{
    FILE  *fp;
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
    char   buf[64];
    char  *end = NULL;
    unsigned long long value;

    if (!read_text(path, buf, sizeof(buf)))
        return fallback;
    value = strtoull(buf, &end, 10);
    if (end == buf)
        return fallback;
    return (uint64_t)value;
}

/** 1 when `name` contains `needle`, case-insensitively. */
static int name_has(const char *name, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    for (i = 0; name[i]; i++) {
        size_t k;
        for (k = 0; k < nlen; k++) {
            if (tolower((unsigned char)name[i + k]) !=
                tolower((unsigned char)needle[k]))
                break;
        }
        if (k == nlen)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Probe 1: the Rockchip vendor driver via debugfs                     */
/* ------------------------------------------------------------------ */

/**
 * Parse the vendor load line, which looks like:
 *   NPU load:  Core0:  12%, Core1:   3%, Core2:   0%,
 * Every "NN%" group after a core label becomes one core reading.
 */
static void parse_rknpu_load(const char *text, npu_t *npu)
{
    const char *p = text;
    double      sum = 0.0;
    int         index = 0;

    while (*p && index < SLH_MAX_NPU_CORES) {
        const char *percent = strchr(p, '%');
        const char *start;
        double      value;

        if (!percent)
            break;
        start = percent;
        while (start > p && (isdigit((unsigned char)start[-1]) || start[-1] == '.'))
            start--;
        if (start == percent) {          /* a bare '%', keep scanning */
            p = percent + 1;
            continue;
        }
        value = strtod(start, NULL);
        if (value < 0.0)
            value = 0.0;
        if (value > 100.0)
            value = 100.0;

        npu->core_load[index++] = value;
        sum += value;
        p = percent + 1;
    }

    if (index > 0) {
        npu->core_count = index;
        npu->load_percent = sum / (double)index;
        npu->load_available = 1;
    }
}

static int probe_rknpu(npu_t *npu)
{
    char path[256];
    char text[512];

    snprintf(path, sizeof(path), "%s/load", RKNPU_DEBUGFS);
    if (!read_text(path, text, sizeof(text)))
        return 0;

    slh_strlcpy(npu->source, path, sizeof(npu->source));
    parse_rknpu_load(text, npu);

    snprintf(path, sizeof(path), "%s/freq", RKNPU_DEBUGFS);
    npu->freq_hz = read_u64(path, 0);

    snprintf(path, sizeof(path), "%s/version", RKNPU_DEBUGFS);
    read_text(path, npu->driver_version, sizeof(npu->driver_version));

    slh_strlcpy(npu->name, "RKNPU", sizeof(npu->name));
    npu->driver_loaded = 1;
    npu->state = NPU_STATE_READY;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Probe 2: a generic devfreq node whose name mentions the NPU         */
/* ------------------------------------------------------------------ */

static int probe_devfreq(npu_t *npu)
{
    DIR           *dir;
    struct dirent *entry;
    int            found = 0;

    dir = opendir(DEVFREQ_SYSFS);
    if (!dir)
        return 0;

    while ((entry = readdir(dir)) != NULL) {
        char path[384];
        char buf[128];

        if (entry->d_name[0] == '.')
            continue;
        if (!name_has(entry->d_name, "npu"))
            continue;

        found = 1;

        snprintf(path, sizeof(path), "%s/%s/cur_freq", DEVFREQ_SYSFS, entry->d_name);
        npu->freq_hz = read_u64(path, npu->freq_hz);
        snprintf(path, sizeof(path), "%s/%s/min_freq", DEVFREQ_SYSFS, entry->d_name);
        npu->freq_min_hz = read_u64(path, 0);
        snprintf(path, sizeof(path), "%s/%s/max_freq", DEVFREQ_SYSFS, entry->d_name);
        npu->freq_max_hz = read_u64(path, 0);

        snprintf(path, sizeof(path), "%s/%s/load", DEVFREQ_SYSFS, entry->d_name);
        if (read_text(path, buf, sizeof(buf))) {
            double value = strtod(buf, NULL);

            /* devfreq reports either a percentage or a raw busy counter;
             * only trust it as a percentage when it is in range. */
            if (value >= 0.0 && value <= 100.0) {
                npu->load_percent = value;
                npu->core_load[0] = value;
                npu->core_count = 1;
                npu->load_available = 1;
                snprintf(npu->source, sizeof(npu->source), "%.130s (devfreq)", path);
            }
        }
        break;
    }
    closedir(dir);
    return found;
}

/* ------------------------------------------------------------------ */
/* Probe 3: is the silicon described at all?                           */
/* ------------------------------------------------------------------ */

/** 1 when any direct child of the device tree is named "*npu*". */
static int devicetree_has_npu(void)
{
    DIR           *dir;
    struct dirent *entry;
    int            found = 0;

    dir = opendir(DEVICE_TREE);
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (name_has(entry->d_name, "npu")) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    if (found)
        return 1;

    /* Rockchip describes the NPU through its clock/power symbols even when
     * the rknpu node itself is not exposed at the top level. */
    dir = opendir(DEVICE_TREE "/__symbols__");
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (name_has(entry->d_name, "npu")) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int dev_rknpu_present(void)
{
    struct stat st;

    return stat("/dev/rknpu", &st) == 0 || stat("/dev/rknpu0", &st) == 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void npu_collect(npu_t *npu)
{
    int i;

    memset(npu, 0, sizeof(*npu));
    npu->load_percent = -1.0;
    for (i = 0; i < SLH_MAX_NPU_CORES; i++)
        npu->core_load[i] = -1.0;

    npu->hw_present = devicetree_has_npu() || dev_rknpu_present();

    if (probe_rknpu(npu)) {
        /* richest source; already set state/name */
    } else if (probe_devfreq(npu)) {
        npu->driver_loaded = 1;
        slh_strlcpy(npu->name, "NPU", sizeof(npu->name));
        npu->state = NPU_STATE_READY;
    } else if (npu->hw_present) {
        npu->state = NPU_STATE_HW_ONLY;
    } else {
        npu->state = NPU_STATE_UNKNOWN;
    }

    /* A human-facing label for the silicon. */
    {
        char soc[96];
        FILE *fp = fopen(DEVICE_TREE "/compatible", "rb");

        soc[0] = '\0';
        if (fp) {
            char   buffer[512];
            size_t len = fread(buffer, 1, sizeof(buffer) - 1, fp);
            size_t offset;

            fclose(fp);
            buffer[len] = '\0';
            for (offset = 0; offset < len; ) {
                const char *entry = buffer + offset;
                size_t      entry_len = strnlen(entry, len - offset);
                const char *comma;

                if (entry_len == 0)
                    break;
                comma = memchr(entry, ',', entry_len);
                if (comma && (size_t)(comma - entry) == 8 &&
                    strncmp(entry, "rockchip", 8) == 0) {
                    size_t k;
                    slh_strlcpy(soc, comma + 1, sizeof(soc));
                    for (k = 0; soc[k]; k++)
                        soc[k] = (char)toupper((unsigned char)soc[k]);
                    break;
                }
                offset += entry_len + 1;
            }
        }
        if (soc[0])
            snprintf(npu->hw_name, sizeof(npu->hw_name), "Rockchip %s NPU", soc);
        else
            slh_strlcpy(npu->hw_name, "NPU", sizeof(npu->hw_name));
    }

    /* Fill frequency limits when the vendor driver did not. */
    if (npu->freq_max_hz == 0)
        npu->freq_max_hz = read_u64(DEVFREQ_SYSFS "/fde40000.npu/max_freq", 0);

    switch (npu->state) {
    case NPU_STATE_READY:
        if (npu->load_available)
            slh_strlcpy(npu->note, "驱动已加载，负载数据正常。", sizeof(npu->note));
        else
            slh_strlcpy(npu->note, "驱动已加载，当前内核未导出负载数据。",
                        sizeof(npu->note));
        break;
    case NPU_STATE_HW_ONLY:
        slh_strlcpy(npu->note,
                    "已检测到 NPU 硬件，但内核未加载 RKNPU 驱动"
                    "（CONFIG_ROCKCHIP_RKNPU）。加载后此处显示实时负载与频率。",
                    sizeof(npu->note));
        break;
    default:
        slh_strlcpy(npu->note, "未检测到 NPU 硬件，该平台可能不带神经网络加速器。",
                    sizeof(npu->note));
        break;
    }
}

void npu_to_json(strbuf_t *sb, const npu_t *npu, int *first)
{
    static const char *state_names[] = { "unknown", "hardware_only", "ready" };
    int i;

    json_add_str(sb, "state",
                 state_names[(int)npu->state <= 2 ? (int)npu->state : 0], first);
    json_add_bool(sb, "hardware_present", npu->hw_present, first);
    json_add_bool(sb, "driver_loaded", npu->driver_loaded, first);
    json_add_bool(sb, "load_available", npu->load_available, first);
    json_add_str(sb, "name", npu->name[0] ? npu->name : NULL, first);
    json_add_str(sb, "hardware", npu->hw_name[0] ? npu->hw_name : NULL, first);
    json_add_str(sb, "driver_version",
                 npu->driver_version[0] ? npu->driver_version : NULL, first);
    json_add_str(sb, "source", npu->source[0] ? npu->source : NULL, first);
    json_add_str(sb, "note", npu->note[0] ? npu->note : NULL, first);

    if (npu->load_percent < 0.0)
        json_add_null(sb, "load_percent", first);
    else
        json_add_double(sb, "load_percent", npu->load_percent, first);

    json_key(sb, "core_load", first);
    sb_appendc(sb, '[');
    for (i = 0; i < npu->core_count; i++) {
        if (i)
            sb_appendc(sb, ',');
        if (npu->core_load[i] < 0.0)
            sb_append(sb, "null");
        else
            sb_appendf(sb, "%.1f", npu->core_load[i]);
    }
    sb_appendc(sb, ']');

    json_add_int(sb, "core_count", npu->core_count, first);
    json_add_uint(sb, "freq_hz", npu->freq_hz, first);
    json_add_uint(sb, "freq_min_hz", npu->freq_min_hz, first);
    json_add_uint(sb, "freq_max_hz", npu->freq_max_hz, first);
}
