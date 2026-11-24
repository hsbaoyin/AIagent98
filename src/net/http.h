/*
 * AIagent98 - minimal HTTP/1.1 client over NetConn.
 */
#ifndef A98_HTTP_H
#define A98_HTTP_H

#include "net.h"
#include "../core/str.h"

typedef struct {
    int status;
    char reason[64];
    char content_type[128];
    char location[512];
    unsigned long content_length;
    int has_length;
    int chunked;
    int connection_close;
    size_t head_len;
} HttpResponse;

/* Parse a complete response head (status line + headers + blank line).
 * `data` need not be NUL-terminated. Returns 0 on success.
 *
 * NOTE: This client does NOT follow redirects. A 3xx response is returned to
 * the caller as-is; the `location` field below is populated from the
 * Location header purely for the caller's own use (e.g. to show a link or to
 * issue a new request). Informational 1xx responses are skipped internally;
 * `status` always describes the final response. */
int http_parse_response_head(const char *data, size_t len, HttpResponse *out,
                             char *err, size_t errcap);

typedef int (*HttpSink)(void *ctx, const char *data, size_t len);

/* Perform one request and feed the response body to `sink` as it arrives.
 * Returns 0 when the response was fully read, -1 on transport/protocol error. */
int http_request(NetConn *conn, const char *method, const Url *url,
                 const char *extra_headers, const char *body, size_t body_len,
                 HttpResponse *resp, HttpSink sink, void *sink_ctx,
                 char *err, size_t errcap);

#endif /* A98_HTTP_H */
