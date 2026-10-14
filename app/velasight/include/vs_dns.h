/****************************************************************************
 * app/velasight/include/vs_dns.h
 *
 * Name resolution through nameservers this application names itself, for a
 * host the network's own resolver cannot see.
 *
 * Why the system resolver is not enough
 * -------------------------------------
 * getaddrinfo() asks the nameserver DHCP supplied, or the router that
 * vs_network_adopt_dns() substitutes when DHCP supplied none, and whoever
 * runs the network chooses that server.  The social cloud's default endpoint
 * exists only in a private DNS view: staging-hlth.xiaomiwear.com is a CNAME
 * into intranet-staging-4798-cn-north.alb.xiaomi.srv, which public resolvers
 * (114.114.114.114, 223.5.5.5, 8.8.8.8) answer with NXDOMAIN.  A network can
 * route to the endpoint and still hand out a resolver like that.  Measured
 * 2026-10-13 on the Wi-Fi this board was provisioned for: wlan0 at
 * 10.116.50.23 reached 10.38.165.121 and the intranet resolvers 10.234.254.10
 * and 10.234.253.10 by ping, and it resolved www.baidu.com, but it could not
 * resolve the cloud.  So the clock sync and every social open failed with
 * "cannot resolve".
 *
 * Why not a second system nameserver
 * ----------------------------------
 * The system resolver asks every server in its list for every name, on every
 * network.  A server that does not answer costs
 * CONFIG_NETDB_DNSCLIENT_RECV_TIMEOUT (30 s) times
 * CONFIG_NETDB_DNSCLIENT_RETRIES (3) per lookup.  On a home network these
 * intranet addresses lead nowhere, so every name the first server could not
 * answer would stall for 90 s.  When the query is made here instead, the cost
 * is bounded by the caller's per-server timeout.  It is paid only for the
 * one host that needs it, and only after getaddrinfo() has already failed.
 *
 * Why not the address itself
 * --------------------------
 * The gateway in front of the cloud routes on the Host header: it answers 403
 * to "Host: 10.38.165.121".  The address behind the name also belongs to the
 * load balancer, which can change it.  Resolving at connect time handles
 * both.  Only the socket address comes from here, and the request still
 * names the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_VELASIGHT_INCLUDE_VS_DNS_H
#define __APP_VELASIGHT_INCLUDE_VS_DNS_H

#include <netinet/in.h>

/****************************************************************************
 * Name: vs_dns_resolve
 *
 * Description:
 *   Ask each nameserver in servers, in order, for host's IPv4 address.  The
 *   first A record any of them returns is the result.  Each server gets one
 *   query, and its reply is waited on for at most timeout_ms.
 *
 *   There is no retry and no cache.  Callers ask only after getaddrinfo()
 *   has failed, and that is rare enough that one extra round trip per new
 *   connection costs less than managing the lifetime of a cached answer.
 *
 *   Blocking.  Not for the UI thread.
 *
 * Input Parameters:
 *   servers    - IPv4 addresses separated by spaces or commas.  An entry
 *                that is not an IPv4 address is logged and skipped.
 *   host       - the name to look up
 *   timeout_ms - how long to wait for each server's reply
 *   addr       - receives the address on success
 *   via        - receives the server that answered, or NULL
 *
 * Returned Value:
 *   0 on success.
 *
 *   -ENOENT when servers names no usable server.  Otherwise the result from
 *   the last server asked: -ENOENT if it answered without an address
 *   (NXDOMAIN, SERVFAIL, or a CNAME chain with no A record), -ETIMEDOUT if
 *   it did not reply, -EBADMSG if its reply did not parse, or the socket
 *   error.
 *
 *   -EINVAL for a NULL argument, a zero timeout, or a host that cannot be
 *   encoded as a DNS name.
 *
 ****************************************************************************/

int vs_dns_resolve(const char *servers, const char *host,
                   unsigned int timeout_ms, struct in_addr *addr,
                   struct in_addr *via);

#endif /* __APP_VELASIGHT_INCLUDE_VS_DNS_H */
