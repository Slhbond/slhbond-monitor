/*
 * netstat.c - interface enumeration, rate calculation and history ring.
 */
#include "netstat.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "log.h"

#define PROC_NET_DEV  "/proc/net/dev"
#define NET_SYSFS     "/sys/class/net"

/* ------------------------------------------------------------------ */
/* Previous-sample store                                               */
/* ------------------------------------------------------------------ */

/*
 * Unlike cpustat (which only needs one global aggregate), traffic rates are
 * per interface and interfaces come and go, so the previous counters are kept
 * in a small name-keyed table guarded by one mutex.
 */
#define SLH_MAX_TRACKED 24

typedef struct {
    char     name[32];
    int      valid;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    int64_t  last_ms;

    double   rx_peak;
    double   tx_peak;

    int      hist_count;
    int      hist_head;
    double   rx_hist[SLH_NET_HISTORY];
    double   tx_hist[SLH_NET_HISTORY];
} net_prev_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static net_prev_t      g_prev[SLH_MAX_TRACKED];
static size_t          g_prev_count;

static net_prev_t *prev_lookup(const char *name, int create)
{
    size_t i;

    for (i = 0; i < g_prev_count; i++) {
        if (strcmp(g_prev[i].name, name) == 0)
            return &g_prev[i];
    }
    if (!create || g_prev_count >= SLH_MAX_TRACKED)
        return NULL;

    memset(&g_prev[g_prev_count], 0, sizeof(g_prev[0]));
    slh_strlcpy(g_prev[g_prev_count].name, name, sizeof(g_prev[0].name));
    return &g_prev[g_prev_count++];
}

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

static int is_loopback_name(const char *n)
{
    return strcmp(n, "lo") == 0;
}

/** Virtual/tunnel interfaces that would only clutter the dashboard. */
static int is_virtual_name(const char *n)
{
    static const char *prefixes[] = {
        "veth", "docker", "br-", "virbr", "vmnet", "tun", "tap", "wg",
        "sit", "ip6tnl", "ip6gre", "gre", "bond", "dummy", "vlan", "zt",
        NULL
    };
    int i;

    for (i = 0; prefixes[i]; i++) {
        size_t len = strlen(prefixes[i]);

        if (strncmp(n, prefixes[i], len) == 0)
            return 1;
    }
    /* eth0.100 style VLANs */
    if (strchr(n, '.') && strncmp(n, "eth", 3) == 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc/net/dev                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[32];
    uint64_t rx_bytes, rx_packets, rx_errs, rx_drop;
    uint64_t tx_bytes, tx_packets, tx_errs, tx_drop;
} dev_row_t;

/**
 * Parse one "iface: rx... tx..." line.
 *
 * /proc/net/dev carries **16** counters per interface - eight receive fields
 * followed by eight transmit fields - so all sixteen have to be scanned before
 * the tx values can be read out of the tail of the array.
 *
 * Returns 1 on success.
 */
static int parse_dev_line(const char *line, dev_row_t *out)
{
    const char        *colon = strchr(line, ':');
    unsigned long long v[16];
    int                n;

    if (!colon)
        return 0;

    memset(out, 0, sizeof(*out));
    {
        size_t len = (size_t)(colon - line);

        while (len > 0 && isspace((unsigned char)line[len - 1]))
            len--;
        {
            size_t start = 0;
            while (start < len && isspace((unsigned char)line[start]))
                start++;
            if (len - start >= sizeof(out->name))
                len = start + sizeof(out->name) - 1;
            memcpy(out->name, line + start, len - start);
            out->name[len - start] = '\0';
        }
    }
    if (out->name[0] == '\0')
        return 0;

    memset(v, 0, sizeof(v));
    n = sscanf(colon + 1,
               "%llu %llu %llu %llu %llu %llu %llu %llu "
               "%llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
               &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
    if (n < 16)
        return 0;

    out->rx_bytes = (uint64_t)v[0];
    out->rx_packets = (uint64_t)v[1];
    out->rx_errs = (uint64_t)v[2];
    out->rx_drop = (uint64_t)v[3];
    out->tx_bytes = (uint64_t)v[8];
    out->tx_packets = (uint64_t)v[9];
    out->tx_errs = (uint64_t)v[10];
    out->tx_drop = (uint64_t)v[11];
    return 1;
}

/* ------------------------------------------------------------------ */
/* sysfs attributes                                                    */
/* ------------------------------------------------------------------ */

static void read_iface_attrs(const char *name, net_iface_t *out)
{
    char path[320];
    char text[64];

    snprintf(path, sizeof(path), "%s/%s/address", NET_SYSFS, name);
    if (slh_read_text(path, text, sizeof(text)))
        slh_strlcpy(out->mac, text, sizeof(out->mac));

    snprintf(path, sizeof(path), "%s/%s/operstate", NET_SYSFS, name);
    if (slh_read_text(path, text, sizeof(text)))
        slh_strlcpy(out->state, text, sizeof(out->state));
    out->is_up = strcmp(out->state, "up") == 0 ||
                 strcmp(out->state, "unknown") == 0;

    snprintf(path, sizeof(path), "%s/%s/speed", NET_SYSFS, name);
    {
        int64_t mbps = slh_read_i64(path, -1);

        if (mbps > 0)
            out->speed_mbps = (uint64_t)mbps;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void netstat_collect(netlist_t *out)
{
    FILE  *fp;
    char   line[512];
    int64_t now = slh_monotonic_ms();

    memset(out, 0, sizeof(*out));
    out->primary = -1;
    out->sampled_at = slh_now_sec();

    fp = fopen(PROC_NET_DEV, "r");
    if (!fp) {
        LOG_WARN("cannot read %s", PROC_NET_DEV);
        return;
    }

    pthread_mutex_lock(&g_lock);

    /* skip the two header lines */
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp)) {
        fclose(fp);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        dev_row_t    row;
        net_iface_t *dst;
        net_prev_t  *prev;
        double       delta_s;

        if (!parse_dev_line(line, &row))
            continue;
        if (out->count >= SLH_MAX_IFACES)
            break;

        dst = &out->iface[out->count];
        memset(dst, 0, sizeof(*dst));
        slh_strlcpy(dst->name, row.name, sizeof(dst->name));

        if (is_loopback_name(row.name)) {
            slh_strlcpy(dst->kind, "loopback", sizeof(dst->kind));
            dst->is_virtual = 1;
        } else if (is_virtual_name(row.name)) {
            slh_strlcpy(dst->kind, "virtual", sizeof(dst->kind));
            dst->is_virtual = 1;
        } else {
            slh_strlcpy(dst->kind, "physical", sizeof(dst->kind));
        }

        read_iface_attrs(row.name, dst);

        dst->s.rx_bytes = row.rx_bytes;
        dst->s.tx_bytes = row.tx_bytes;
        dst->s.rx_packets = row.rx_packets;
        dst->s.tx_packets = row.tx_packets;
        dst->s.rx_errors = row.rx_errs;
        dst->s.tx_errors = row.tx_errs;
        dst->s.rx_dropped = row.rx_drop;
        dst->s.tx_dropped = row.tx_drop;

        prev = prev_lookup(row.name, 1);
        if (prev) {
            if (prev->valid && now > prev->last_ms) {
                delta_s = (double)(now - prev->last_ms) / 1000.0;
                if (delta_s > 0.0) {
                    dst->s.rx_bps = row.rx_bytes >= prev->rx_bytes
                        ? (double)(row.rx_bytes - prev->rx_bytes) / delta_s : 0.0;
                    dst->s.tx_bps = row.tx_bytes >= prev->tx_bytes
                        ? (double)(row.tx_bytes - prev->tx_bytes) / delta_s : 0.0;
                }
            }

            /* Update peaks and push onto the history ring. */
            if (dst->s.rx_bps > prev->rx_peak)
                prev->rx_peak = dst->s.rx_bps;
            if (dst->s.tx_bps > prev->tx_peak)
                prev->tx_peak = dst->s.tx_bps;
            dst->s.rx_peak_bps = prev->rx_peak;
            dst->s.tx_peak_bps = prev->tx_peak;

            if (prev->valid) {
                prev->rx_hist[prev->hist_head] = dst->s.rx_bps;
                prev->tx_hist[prev->hist_head] = dst->s.tx_bps;
                prev->hist_head = (prev->hist_head + 1) % SLH_NET_HISTORY;
                if (prev->hist_count < SLH_NET_HISTORY)
                    prev->hist_count++;
            }

            dst->s.hist_count = prev->hist_count;
            dst->s.hist_head = prev->hist_head;
            memcpy(dst->s.rx_hist, prev->rx_hist, sizeof(dst->s.rx_hist));
            memcpy(dst->s.tx_hist, prev->tx_hist, sizeof(dst->s.tx_hist));

            prev->valid = 1;
            prev->rx_bytes = row.rx_bytes;
            prev->tx_bytes = row.tx_bytes;
            prev->last_ms = now;
        }

        out->count++;
    }

    pthread_mutex_unlock(&g_lock);
    fclose(fp);

    /* Prefer an up, physical interface carrying the most traffic. */
    {
        size_t i;
        uint64_t best_traffic = 0;

        for (i = 0; i < out->count; i++) {
            net_iface_t *n = &out->iface[i];
            uint64_t traffic;

            if (n->is_virtual || !n->is_up)
                continue;
            traffic = n->s.rx_bytes + n->s.tx_bytes;
            if (out->primary < 0 || traffic > best_traffic) {
                out->primary = (int)i;
                best_traffic = traffic;
            }
        }
        /* Fall back to any non-loopback interface. */
        if (out->primary < 0) {
            for (i = 0; i < out->count; i++) {
                if (strcmp(out->iface[i].kind, "loopback") != 0) {
                    out->primary = (int)i;
                    break;
                }
            }
        }
    }
}

void netstat_to_json(strbuf_t *sb, const netlist_t *l, int *first)
{
    size_t i;

    json_add_int(sb, "primary_index", l->primary, first);
    json_add_int(sb, "interface_count", (long long)l->count, first);
    json_add_int(sb, "history_length", SLH_NET_HISTORY, first);

    json_key(sb, "interfaces", first);
    sb_appendc(sb, '[');
    for (i = 0; i < l->count; i++) {
        const net_iface_t *n = &l->iface[i];
        int f = 1;
        int k;

        if (i)
            sb_appendc(sb, ',');
        sb_appendc(sb, '{');

        json_add_str(sb, "name", n->name, &f);
        json_add_str(sb, "mac", n->mac[0] ? n->mac : NULL, &f);
        json_add_str(sb, "state", n->state[0] ? n->state : NULL, &f);
        json_add_str(sb, "kind", n->kind, &f);
        json_add_bool(sb, "up", n->is_up, &f);
        json_add_bool(sb, "virtual", n->is_virtual, &f);
        json_add_uint(sb, "speed_mbps", n->speed_mbps, &f);
        json_add_uint(sb, "rx_bytes", n->s.rx_bytes, &f);
        json_add_uint(sb, "tx_bytes", n->s.tx_bytes, &f);
        json_add_uint(sb, "rx_packets", n->s.rx_packets, &f);
        json_add_uint(sb, "tx_packets", n->s.tx_packets, &f);
        json_add_uint(sb, "rx_errors", n->s.rx_errors, &f);
        json_add_uint(sb, "tx_errors", n->s.tx_errors, &f);
        json_add_uint(sb, "rx_dropped", n->s.rx_dropped, &f);
        json_add_uint(sb, "tx_dropped", n->s.tx_dropped, &f);
        json_add_double(sb, "rx_bps", n->s.rx_bps, &f);
        json_add_double(sb, "tx_bps", n->s.tx_bps, &f);
        json_add_double(sb, "rx_peak_bps", n->s.rx_peak_bps, &f);
        json_add_double(sb, "tx_peak_bps", n->s.tx_peak_bps, &f);

        /* Histories, oldest first, so the waveform reads left to right. */
        json_key(sb, "rx_history", &f);
        sb_appendc(sb, '[');
        for (k = 0; k < n->s.hist_count; k++) {
            int idx = (n->s.hist_head - n->s.hist_count + k + SLH_NET_HISTORY)
                      % SLH_NET_HISTORY;
            if (k)
                sb_appendc(sb, ',');
            sb_appendf(sb, "%.0f", n->s.rx_hist[idx]);
        }
        sb_appendc(sb, ']');

        json_key(sb, "tx_history", &f);
        sb_appendc(sb, '[');
        for (k = 0; k < n->s.hist_count; k++) {
            int idx = (n->s.hist_head - n->s.hist_count + k + SLH_NET_HISTORY)
                      % SLH_NET_HISTORY;
            if (k)
                sb_appendc(sb, ',');
            sb_appendf(sb, "%.0f", n->s.tx_hist[idx]);
        }
        sb_appendc(sb, ']');

        sb_appendc(sb, '}');
    }
    sb_appendc(sb, ']');
}
