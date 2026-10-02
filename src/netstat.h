/*
 * netstat.h - per-interface traffic counters plus a short rolling history.
 *
 * The history exists so the dashboard can draw a real waveform: rates are
 * derived from /proc/net/dev deltas on every collect, and the last
 * SLH_NET_HISTORY samples are kept in a ring buffer. The series therefore
 * fills in as the daemon is polled, and survives nothing beyond the process
 * lifetime - by design, this is a live view, not a metrics database.
 */
#ifndef SLH_NETSTAT_H
#define SLH_NETSTAT_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define SLH_NET_HISTORY 48
#define SLH_MAX_IFACES  12

typedef struct {
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_packets, tx_packets;
    uint64_t rx_errors, tx_errors;
    uint64_t rx_dropped, tx_dropped;

    double   rx_bps;            /* bytes/s since the previous sample */
    double   tx_bps;
    double   rx_peak_bps;
    double   tx_peak_bps;

    int      hist_count;        /* valid entries, <= SLH_NET_HISTORY */
    int      hist_head;         /* ring write index                  */
    double   rx_hist[SLH_NET_HISTORY];
    double   tx_hist[SLH_NET_HISTORY];
} net_series_t;

typedef struct {
    char     name[32];
    char     mac[32];
    char     state[24];         /* operstate, e.g. "up"              */
    char     kind[16];          /* "physical" / "virtual" / "loopback" */
    uint64_t speed_mbps;        /* 0 when the driver does not report  */
    int      is_up;
    int      is_virtual;
    net_series_t s;
} net_iface_t;

typedef struct {
    size_t  count;
    net_iface_t iface[SLH_MAX_IFACES];
    int     primary;            /* index of the preferred interface, -1 none */
    int64_t sampled_at;         /* epoch seconds of this sample */
} netlist_t;

/** Sample /proc/net/dev and the per-interface sysfs attributes. */
void netstat_collect(netlist_t *out);

/** Emit the whole network view as JSON members, including the histories. */
void netstat_to_json(strbuf_t *sb, const netlist_t *l, int *first);

#endif /* SLH_NETSTAT_H */
