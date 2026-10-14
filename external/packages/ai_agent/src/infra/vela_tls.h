/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of HTTP response header section we buffer internally */
#define VELA_TLS_HDR_BUF  4096

/* Return codes (negative on error, HTTP status code on success) */
#define VELA_TLS_ERR_CONNECT   -1
#define VELA_TLS_ERR_HANDSHAKE -2
#define VELA_TLS_ERR_WRITE     -3
#define VELA_TLS_ERR_READ      -4
#define VELA_TLS_ERR_OVERFLOW  -5
#define VELA_TLS_ERR_SINK      -6  /* the caller's body sink refused a write */
#define VELA_TLS_ERR_PROTOCOL  -7  /* malformed chunked framing            */
#define VELA_TLS_ERR_TIMEOUT   -8  /* a bounded request ran out of time;
                                    * vela_https_request_bounded() only */

/**
 * Extra header entry; pass an array terminated by {NULL, NULL}.
 */
typedef struct {
    const char *name;
    const char *value;
} vela_header_t;

/**
 * Receiver for response body bytes, for callers whose body does not fit in
 * memory.
 *
 * Called repeatedly, in order, with whatever has arrived; the pointer is only
 * valid for the duration of the call.  Transfer-Encoding is already undone,
 * so what arrives here is payload.
 *
 * @return 0 to continue, negative to abort the transfer.  An abort is
 *         reported to the original caller as VELA_TLS_ERR_SINK.
 */
typedef int (*vela_body_sink_t)(void *arg, const void *data, size_t len);

/**
 * Perform a complete HTTPS request and collect the response body.
 *
 * @param host        Hostname (e.g. "api.anthropic.com")
 * @param port        Port string (e.g. "443")
 * @param method      "GET", "POST", etc.
 * @param path        URL path starting with '/' (e.g. "/v1/messages")
 * @param headers     NULL-terminated array of extra request headers, or NULL
 * @param body        Request body bytes, or NULL
 * @param body_len    Length of body; 0 if no body
 * @param resp_buf    Caller-supplied buffer for response body
 * @param resp_cap    Capacity of resp_buf (bytes)
 *
 * @return HTTP status code (200, 400, etc.) on success, negative VELA_TLS_ERR_* on failure
 *
 * The response body is written to resp_buf as a NUL-terminated string.
 * If the body is larger than resp_cap-1, it is silently truncated.
 */
int vela_https_request(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,   /* NULL-terminated array or NULL */
    const char       *body,
    size_t            body_len,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len  /* optional: actual body bytes written, or NULL */
);

/**
 * As vela_https_request(), with every network wait of this one request
 * bounded by timeout_ms.
 *
 * The defaults suit an LLM that can think for a minute: a receive waits up
 * to AGENT_LLM_SOCKET_TIMEOUT_SEC, a send for as long as TCP keeps
 * retransmitting, and a new connection for as long as the stack keeps
 * resending its SYN.  On a link that stalls mid-request that holds the
 * caller for minutes.  Here the TCP connect of a new connection, each
 * handshake read, and each send and receive give up after timeout_ms.
 *
 * A request that fails after waiting out its bound returns
 * VELA_TLS_ERR_TIMEOUT and is not retried on a new connection, which the
 * stale-connection retry would otherwise do.  A pooled connection gets its
 * default timeouts back before it is released, so later requests on it
 * are unaffected.
 *
 * This bounds waiting, not the request: a transfer that keeps making
 * progress is not cut off, and name resolution goes through the system
 * resolver unbounded (its cache normally answers for a host in use).  So
 * the worst case is a few multiples of timeout_ms, not timeout_ms.
 *
 * @param timeout_ms  Per-wait bound.  0 behaves as vela_https_request().
 */
int vela_https_request_bounded(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,
    const char       *body,
    size_t            body_len,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len,
    unsigned int      timeout_ms
);

/**
 * As vela_https_request(), but the response body is handed to a callback as it
 * arrives instead of being collected into a buffer.
 *
 * For bodies too large to hold: memory use is one internal 8 KB read buffer
 * regardless of body size, so a multi-megabyte download costs the same as a
 * small one.  The sink is called from this thread, synchronously, so a sink
 * that blocks (writing to a file, feeding a DAC) applies backpressure to the
 * socket rather than needing a queue.
 *
 * Everything else -- the connection pool, keep-alive, redirect-free semantics,
 * the return value -- behaves as vela_https_request().  A chunked body is
 * de-chunked incrementally before it reaches the sink; such a connection is
 * not returned to the pool, because its trailer is deliberately not consumed.
 *
 * @param sink        Receives body bytes.  Required.
 * @param sink_arg    Passed through to sink untouched.
 * @param out_body_len Optional: total payload bytes handed to sink.
 *
 * @return HTTP status code on success, negative VELA_TLS_ERR_* on failure.
 */
int vela_https_request_stream(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,
    const char       *body,
    size_t            body_len,
    vela_body_sink_t  sink,
    void             *sink_arg,
    size_t           *out_body_len
);

/**
 * Convenience wrapper for GET with no extra headers or body.
 */
int vela_https_get(const char *host, const char *port, const char *path,
                   char *resp_buf, size_t resp_cap);

/**
 * Convenience wrapper for POST with Content-Type: application/json.
 */
int vela_https_post_json(const char *host, const char *port, const char *path,
                         const vela_header_t *extra_headers,
                         const char *json_body,
                         char *resp_buf, size_t resp_cap);

/**
 * Plain HTTP (no TLS) POST with Content-Type: application/json.
 * Used for internal/intranet endpoints that don't support HTTPS.
 */
int vela_http_post_json(const char *host, const char *port, const char *path,
                        const vela_header_t *extra_headers,
                        const char *json_body,
                        char *resp_buf, size_t resp_cap);

/**
 * Send a HEAD request and extract the value of the "Date:" response header.
 *
 * @param host     Hostname (e.g. "api.deepseek.com")
 * @param port     Port string (e.g. "443")
 * @param path     Request path (e.g. "/")
 * @param date_out Caller-supplied buffer to receive the Date header value
 * @param date_cap Capacity of date_out
 *
 * @return 0 on success, negative on error
 */
int vela_https_head_date(const char *host, const char *port, const char *path,
                         char *date_out, size_t date_cap);

/**
 * Release all TLS connection pool resources.
 * Call during shutdown to free mbedtls contexts.
 */
void vela_tls_pool_cleanup(void);

#ifdef __cplusplus
}
#endif
