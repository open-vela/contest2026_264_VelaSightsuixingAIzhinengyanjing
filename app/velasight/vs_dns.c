/****************************************************************************
 * app/velasight/vs_dns.c
 *
 * One A query to one named nameserver at a time.  See include/vs_dns.h for
 * why the system resolver is not enough.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "include/vs_dns.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RFC 1035's limit for a UDP message.  No EDNS option is sent, so no server
 * answers with more.  A reply that does not fit is truncated by recv(), and
 * the bounds checks then refuse it instead of reading past the buffer.
 */

#define DNS_MSG_MAX    512
#define DNS_HEADER_LEN 12
#define DNS_PORT       53
#define DNS_TYPE_A     1
#define DNS_CLASS_IN   1

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t dns_get16(const uint8_t *p)
{
  return (uint16_t)((p[0] << 8) | p[1]);
}

static int64_t dns_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * Name: dns_put_query
 *
 * Description:
 *   Encode a recursive A/IN question for host into buf, with a zero ID that
 *   dns_ask() fills in per server.  A trailing dot is accepted and names the
 *   same host.
 *
 * Returned Value:
 *   The message length, or -EINVAL for a name that is not a sequence of
 *   1-63 byte labels or does not fit in cap.
 *
 ****************************************************************************/

static int dns_put_query(uint8_t *buf, size_t cap, const char *host)
{
  const char *label = host;
  size_t off = DNS_HEADER_LEN;

  if (cap < DNS_HEADER_LEN)
    {
      return -EINVAL;
    }

  memset(buf, 0, DNS_HEADER_LEN);
  buf[2] = 0x01;                        /* RD: the server recurses */
  buf[5] = 0x01;                        /* QDCOUNT 1 */

  while (*label != '\0')
    {
      const char *dot = strchr(label, '.');
      size_t len = dot != NULL ? (size_t)(dot - label) : strlen(label);

      /* The 5 is the root label, QTYPE and QCLASS that must still fit. */

      if (len == 0 || len > 63 || off + 1 + len + 5 > cap)
        {
          return -EINVAL;
        }

      buf[off++] = (uint8_t)len;
      memcpy(buf + off, label, len);
      off += len;

      if (dot == NULL)
        {
          break;
        }

      label = dot + 1;
    }

  if (off == DNS_HEADER_LEN)
    {
      return -EINVAL;
    }

  buf[off++] = 0;
  buf[off++] = 0;
  buf[off++] = DNS_TYPE_A;
  buf[off++] = 0;
  buf[off++] = DNS_CLASS_IN;
  return (int)off;
}

/****************************************************************************
 * Name: dns_skip_name
 *
 * Description:
 *   The offset just past the encoded name at off: a run of labels that ends
 *   in the root label or in a compression pointer.  The pointer is not
 *   followed.  Nothing here needs the name itself, only where the record
 *   after it begins.  Not following pointers also means a pointer loop
 *   cannot trap the parser.
 *
 ****************************************************************************/

static int dns_skip_name(const uint8_t *msg, size_t len, size_t off)
{
  while (off < len)
    {
      uint8_t c = msg[off];

      if (c == 0)
        {
          return (int)(off + 1);
        }

      if ((c & 0xc0) == 0xc0)
        {
          return off + 2 <= len ? (int)(off + 2) : -EBADMSG;
        }

      if ((c & 0xc0) != 0)
        {
          return -EBADMSG;              /* 0x40/0x80 label types: unused */
        }

      off += 1 + (size_t)c;
    }

  return -EBADMSG;
}

/****************************************************************************
 * Name: dns_first_a
 *
 * Description:
 *   The first A/IN record in the answer section of the reply in msg.
 *
 *   CNAME records ahead of it are stepped over rather than chased.  A
 *   recursive server puts the whole chain in one answer, and that is how the
 *   name this exists for arrives: a CNAME into a private zone, followed by
 *   the A record the chain ends in.
 *
 * Returned Value:
 *   0 with *addr set.  -EAGAIN for a datagram that is not the reply to this
 *   query; the caller keeps waiting past it.  -ENOENT when the server
 *   answered without an address.  -EBADMSG when the reply does not parse.
 *
 ****************************************************************************/

static int dns_first_a(const uint8_t *msg, size_t len, uint16_t id,
                       struct in_addr *addr)
{
  unsigned int questions;
  unsigned int answers;
  int off;

  if (len < DNS_HEADER_LEN || dns_get16(msg) != id || (msg[2] & 0x80) == 0)
    {
      return -EAGAIN;
    }

  if ((msg[3] & 0x0f) != 0)
    {
      return -ENOENT;                   /* RCODE: NXDOMAIN, SERVFAIL, ... */
    }

  questions = dns_get16(msg + 4);
  answers   = dns_get16(msg + 6);
  off       = DNS_HEADER_LEN;

  while (questions-- > 0)
    {
      off = dns_skip_name(msg, len, (size_t)off);
      if (off < 0 || (size_t)off + 4 > len)
        {
          return -EBADMSG;
        }

      off += 4;                         /* QTYPE, QCLASS */
    }

  while (answers-- > 0)
    {
      uint16_t rdlen;

      off = dns_skip_name(msg, len, (size_t)off);
      if (off < 0 || (size_t)off + 10 > len)
        {
          return -EBADMSG;
        }

      /* TYPE, CLASS, TTL, RDLENGTH, then RDATA. */

      rdlen = dns_get16(msg + off + 8);
      if ((size_t)off + 10 + rdlen > len)
        {
          return -EBADMSG;
        }

      if (dns_get16(msg + off) == DNS_TYPE_A &&
          dns_get16(msg + off + 2) == DNS_CLASS_IN && rdlen == 4)
        {
          memcpy(&addr->s_addr, msg + off + 10, 4);
          return 0;
        }

      off += 10 + rdlen;
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: dns_ask
 *
 * Description:
 *   Send the encoded query to one server and wait at most timeout_ms for
 *   its reply, which is read into reply (DNS_MSG_MAX bytes).
 *
 ****************************************************************************/

static int dns_ask(const struct in_addr *server, uint8_t *query, int qlen,
                   uint8_t *reply, unsigned int timeout_ms,
                   struct in_addr *addr)
{
  struct sockaddr_in to;
  struct timespec ts;
  struct pollfd pfd;
  int64_t deadline;
  ssize_t sent;
  uint16_t id;
  int sock;
  int ret;

  /* The ID is picked the same way the system resolver picks it
   * (lib_dnsquery.c's dns_alloc_id()), for the same reason.  The socket is
   * connected to the one server asked, so the ID only has to tell this
   * query's reply apart from a stray datagram; it does not have to resist
   * being guessed.
   */

  clock_gettime(CLOCK_MONOTONIC, &ts);
  id = (uint16_t)((uint32_t)ts.tv_nsec + ((uint32_t)ts.tv_nsec >> 16));
  query[0] = (uint8_t)(id >> 8);
  query[1] = (uint8_t)id;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      return -errno;
    }

  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port   = htons(DNS_PORT);
  to.sin_addr   = *server;

  /* Connected, so the stack discards datagrams from any other source. */

  if (connect(sock, (struct sockaddr *)&to, sizeof(to)) < 0)
    {
      ret = -errno;
      goto out;
    }

  sent = send(sock, query, (size_t)qlen, 0);
  if (sent != qlen)
    {
      ret = sent < 0 ? -errno : -EIO;
      goto out;
    }

  deadline = dns_now_ms() + timeout_ms;

  for (; ; )
    {
      int64_t left = deadline - dns_now_ms();
      ssize_t got;

      if (left <= 0)
        {
          ret = -ETIMEDOUT;
          break;
        }

      pfd.fd      = sock;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, (int)left);
      if (ret < 0 && errno == EINTR)
        {
          continue;
        }

      if (ret <= 0)
        {
          ret = ret == 0 ? -ETIMEDOUT : -errno;
          break;
        }

      got = recv(sock, reply, DNS_MSG_MAX, 0);
      if (got < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      ret = dns_first_a(reply, (size_t)got, id, addr);
      if (ret != -EAGAIN)
        {
          break;
        }
    }

out:
  close(sock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_dns_resolve(const char *servers, const char *host,
                   unsigned int timeout_ms, struct in_addr *addr,
                   struct in_addr *via)
{
  const char *p;
  uint8_t *buf;
  int qlen;
  int ret = -ENOENT;

  if (servers == NULL || host == NULL || addr == NULL || timeout_ms == 0)
    {
      return -EINVAL;
    }

  /* The query in the first half, each reply in the second.  On the heap
   * because this runs inside cloud_connect(), on whichever thread opens the
   * connection, and some of those threads have 4 KiB pthread stacks.
   *
   * Encoded once, before any server is asked, so a name that cannot be sent
   * fails as -EINVAL here rather than once per server.
   */

  buf = malloc(2 * DNS_MSG_MAX);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  qlen = dns_put_query(buf, DNS_MSG_MAX, host);
  if (qlen < 0)
    {
      free(buf);
      return qlen;
    }

  for (p = servers; ; )
    {
      char text[INET_ADDRSTRLEN];
      struct in_addr server;
      const char *entry;
      size_t n;

      p += strspn(p, ", ");
      n = strcspn(p, ", ");
      if (n == 0)
        {
          break;
        }

      entry = p;
      p += n;

      if (n < sizeof(text))
        {
          memcpy(text, entry, n);
          text[n] = '\0';
        }

      if (n >= sizeof(text) || inet_pton(AF_INET, text, &server) != 1)
        {
          printf("vs_dns: \"%.*s\" is not an IPv4 address, skipped\n",
                 (int)n, entry);
          continue;
        }

      ret = dns_ask(&server, buf, qlen, buf + DNS_MSG_MAX, timeout_ms, addr);
      if (ret == 0)
        {
          if (via != NULL)
            {
              *via = server;
            }

          break;
        }
    }

  free(buf);
  return ret;
}
