/*
 * netinfo.h - IPv4 / IPv6 address discovery for the local host.
 */
#ifndef SLH_NETINFO_H
#define SLH_NETINFO_H

#include <stddef.h>

#include "util.h"

#define SLH_MAX_ADDRS 64
#define SLH_IFNAME_LEN 32
#define SLH_ADDR_LEN 64

/** One address bound to one interface. */
typedef struct {
    char         ifname[SLH_IFNAME_LEN];
    char         addr[SLH_ADDR_LEN];  /* presentation form, no scope suffix */
    unsigned int prefix_len;
    int          is_loopback;
    int          is_link_local;
} slh_addr_t;

typedef struct {
    slh_addr_t ipv4[SLH_MAX_ADDRS];
    size_t     ipv4_count;
    char       ipv4_primary[SLH_ADDR_LEN];   /* "" when none was found */
    char       ipv4_ifname[SLH_IFNAME_LEN];

    slh_addr_t ipv6[SLH_MAX_ADDRS];
    size_t     ipv6_count;
    char       ipv6_primary[SLH_ADDR_LEN];   /* "" when none -> UI shows 未获取 */
    char       ipv6_ifname[SLH_IFNAME_LEN];
} netinfo_t;

/**
 * Enumerate the host's addresses with getifaddrs(3).
 * Always succeeds; when enumeration fails every list is left empty so the UI
 * can fall back to its "未获取" placeholder. Returns the number of addresses
 * recorded across both families.
 */
size_t netinfo_collect(netinfo_t *ni);

/** Emit the netinfo members into an object under construction. */
void netinfo_to_json(strbuf_t *sb, const netinfo_t *ni, int *first);

#endif /* SLH_NETINFO_H */
