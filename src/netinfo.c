/*
 * netinfo.c - address enumeration built on getifaddrs(3).
 *
 * Selection policy for the "primary" address shown in the dashboard:
 *   1. skip loopback and IPv6 link-local addresses;
 *   2. prefer an interface that is UP and RUNNING;
 *   3. otherwise fall back to whatever was found, so the panel is never
 *      empty on an unusual host.
 */
#include "netinfo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "json.h"
#include "log.h"

static int is_ipv6_link_local(const struct in6_addr *a)
{
    return IN6_IS_ADDR_LINKLOCAL(a) || IN6_IS_ADDR_SITELOCAL(a);
}

/** Higher score wins. Negative means "never prefer this address". */
static int score_address(const slh_addr_t *a, int is_v6)
{
    int score = 0;

    if (a->is_loopback)
        return -1;
    if (is_v6 && a->is_link_local)
        return 0;
    score = 10;
    if (is_v6)
        score += 1;
    return score;
}

static void consider(netinfo_t *ni, const slh_addr_t *addr, int is_v6, int *best_score)
{
    slh_addr_t *list;
    size_t     *count;
    char       *primary;
    char       *primary_if;
    int         score;

    if (is_v6) {
        list = ni->ipv6;
        count = &ni->ipv6_count;
        primary = ni->ipv6_primary;
        primary_if = ni->ipv6_ifname;
    } else {
        list = ni->ipv4;
        count = &ni->ipv4_count;
        primary = ni->ipv4_primary;
        primary_if = ni->ipv4_ifname;
    }

    if (*count < SLH_MAX_ADDRS)
        list[(*count)++] = *addr;

    score = score_address(addr, is_v6);
    if (score > *best_score) {
        *best_score = score;
        slh_strlcpy(primary, addr->addr, SLH_ADDR_LEN);
        slh_strlcpy(primary_if, addr->ifname, SLH_IFNAME_LEN);
    }
}

/** Append "%%ifname" for link-local IPv6 so the value stays usable. */
static void format_v6(const struct sockaddr_in6 *sin6, const char *ifname,
                      char *out, size_t out_size)
{
    if (!inet_ntop(AF_INET6, &sin6->sin6_addr, out, (socklen_t)out_size)) {
        slh_strlcpy(out, "", out_size);
        return;
    }
    if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) && ifname && ifname[0]) {
        slh_strlcat(out, "%", out_size);
        slh_strlcat(out, ifname, out_size);
    }
}

size_t netinfo_collect(netinfo_t *ni)
{
    struct ifaddrs *head = NULL;
    struct ifaddrs *ifa;
    int best_v4 = -1;
    int best_v6 = -1;

    memset(ni, 0, sizeof(*ni));

    if (getifaddrs(&head) != 0) {
        /*
         * The usual cause is a service sandbox that omits AF_NETLINK:
         * getifaddrs(3) enumerates addresses over a netlink socket, so
         * systemd's RestrictAddressFamilies must list AF_NETLINK alongside
         * AF_INET/AF_INET6 or every address silently disappears.
         */
        LOG_WARN("getifaddrs failed (%s); no addresses will be reported. "
                 "Under systemd, check that RestrictAddressFamilies includes "
                 "AF_NETLINK.", strerror(errno));
        return 0;
    }

    for (ifa = head; ifa; ifa = ifa->ifa_next) {
        slh_addr_t entry;
        const char *ifname = ifa->ifa_name ? ifa->ifa_name : "";

        if (!ifa->ifa_addr)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        memset(&entry, 0, sizeof(entry));
        slh_strlcpy(entry.ifname, ifname, SLH_IFNAME_LEN);
        entry.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) ? 1 : 0;
        entry.prefix_len = 0;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;

            if (!inet_ntop(AF_INET, &sin->sin_addr, entry.addr, SLH_ADDR_LEN))
                continue;
            if (ifa->ifa_netmask) {
                const struct sockaddr_in *mask =
                    (const struct sockaddr_in *)ifa->ifa_netmask;
                uint32_t m = ntohl(mask->sin_addr.s_addr);
                while (m) {
                    entry.prefix_len += (m & 0x80000000u) ? 1u : 0u;
                    m <<= 1;
                }
            }
            consider(ni, &entry, 0, &best_v4);
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            const struct sockaddr_in6 *sin6 =
                (const struct sockaddr_in6 *)ifa->ifa_addr;

            entry.is_link_local = is_ipv6_link_local(&sin6->sin6_addr) ? 1 : 0;
            format_v6(sin6, ifname, entry.addr, SLH_ADDR_LEN);
            if (!entry.addr[0])
                continue;
            if (ifa->ifa_netmask) {
                const struct sockaddr_in6 *mask6 =
                    (const struct sockaddr_in6 *)ifa->ifa_netmask;
                int i;
                for (i = 0; i < 16; i++) {
                    unsigned char byte = mask6->sin6_addr.s6_addr[i];
                    while (byte) {
                        entry.prefix_len += (byte & 0x80u) ? 1u : 0u;
                        byte = (unsigned char)(byte << 1);
                    }
                }
            }
            consider(ni, &entry, 1, &best_v6);
        }
    }

    freeifaddrs(head);
    return ni->ipv4_count + ni->ipv6_count;
}

void netinfo_to_json(strbuf_t *sb, const netinfo_t *ni, int *first)
{
    json_add_str(sb, "ipv4", ni->ipv4_primary[0] ? ni->ipv4_primary : NULL, first);
    json_add_str(sb, "ipv4_interface",
                 ni->ipv4_ifname[0] ? ni->ipv4_ifname : NULL, first);
    json_add_str(sb, "ipv6", ni->ipv6_primary[0] ? ni->ipv6_primary : NULL, first);
    json_add_str(sb, "ipv6_interface",
                 ni->ipv6_ifname[0] ? ni->ipv6_ifname : NULL, first);

    {
        const char *items[SLH_MAX_ADDRS];
        size_t i;

        for (i = 0; i < ni->ipv4_count; i++)
            items[i] = ni->ipv4[i].addr;
        json_add_str_array(sb, "ipv4_all", items, ni->ipv4_count, first);

        for (i = 0; i < ni->ipv6_count; i++)
            items[i] = ni->ipv6[i].addr;
        json_add_str_array(sb, "ipv6_all", items, ni->ipv6_count, first);
    }
}
