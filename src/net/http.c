#include "http.h"
#include "body.h"
#include "../core/log.h"

#include <errno.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int is_tchar(char c)
{
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    return c == '-' || c == '_' || c == '.' || c == '!' || c == '#' ||
           c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
           c == '+' || c == '^' || c == '`' || c == '|' || c == '~';
}

int http_parse_response_head(const char *data, size_t len, HttpResponse *out,
                             char *err, size_t errcap)
{
    size_t i;
    size_t head_end = 0;
    size_t line_start;
    int found_end = 0;
    int line_no = 0;

    if (out == NULL || data == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (i + 1 < len && data[i + 1] == '\n') {
                head_end = i + 2;
                found_end = 1;
                break;
            }
            if (i >= 2 && data[i - 1] == '\r' && data[i - 2] == '\n') {
                head_end = i + 1;
                found_end = 1;
                break;
            }
        }
    }
    if (!found_end) {
        set_err(err, errcap, "incomplete HTTP response head");
        return -1;
    }
    out->head_len = head_end;

    line_start = 0;
    for (i = 0; i <= head_end && line_start < head_end; i++) {
        if (i == head_end || data[i] == '\n') {
            size_t end = i;
            if (end > line_start && data[end - 1] == '\r') {
                end--;
            }
            if (line_no == 0) {
                /* Status line: HTTP/1.x SP status SP reason */
                size_t p = line_start;
                int status = 0;
                if (end - line_start < 12 ||
                    memcmp(data + line_start, "HTTP/", 5) != 0) {
                    set_err(err, errcap, "not an HTTP response");
                    return -1;
                }
                while (p < end && data[p] != ' ') {
                    p++;
                }
                if (p >= end || end - p < 4) {
                    set_err(err, errcap, "malformed HTTP status line");
                    return -1;
                }
                p++;
                if (!(data[p] >= '0' && data[p] <= '9') ||
                    !(data[p + 1] >= '0' && data[p + 1] <= '9') ||
                    !(data[p + 2] >= '0' && data[p + 2] <= '9')) {
                    set_err(err, errcap, "malformed HTTP status code");
                    return -1;
                }
                status = (data[p] - '0') * 100 + (data[p + 1] - '0') * 10 +
                         (data[p + 2] - '0');
                out->status = status;
                p += 3;
                while (p < end && data[p] == ' ') {
                    p++;
                }
                {
                    size_t n = end - p;
                    if (n >= sizeof(out->reason)) {
                        n = sizeof(out->reason) - 1;
                    }
                    memcpy(out->reason, data + p, n);
                    out->reason[n] = '\0';
                }
            } else if (end > line_start) {
                size_t colon = line_start;
                char name[64];
                char value[1024];
                size_t nlen;
                size_t vstart;
                size_t vlen;

                while (colon < end && data[colon] != ':') {
                    colon++;
                }
                if (colon >= end) {
                    set_err(err, errcap, "malformed HTTP header line");
                    return -1;
                }
                nlen = colon - line_start;
                if (nlen == 0 || nlen >= sizeof(name)) {
                    set_err(err, errcap, "invalid HTTP header name");
                    return -1;
                }
                memcpy(name, data + line_start, nlen);
                name[nlen] = '\0';
                vstart = colon + 1;
                while (vstart < end && (data[vstart] == ' ' || data[vstart] == '\t')) {
                    vstart++;
                }
                vlen = end - vstart;
                if (vlen >= sizeof(value)) {
                    /* Truncating used to turn a long Content-Length or
                     * Transfer-Encoding into a confusing parse error. */
                    set_err(err, errcap, "HTTP header value is too long");
                    return -1;
                }
                memcpy(value, data + vstart, vlen);
                value[vlen] = '\0';
                while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
                    value[--vlen] = '\0';
                }

                if (a98_strcasecmp(name, "Content-Length") == 0) {
                    char *endptr = NULL;
                    unsigned long cl;

                    /* strtoul("-1") would wrap to ULONG_MAX; reject signs. */
                    if (value[0] == '-' || value[0] == '+') {
                        set_err(err, errcap, "invalid Content-Length");
                        return -1;
                    }
                    errno = 0;
                    cl = strtoul(value, &endptr, 10);
                    if (errno == ERANGE || endptr == value || *endptr != '\0') {
                        set_err(err, errcap, "invalid Content-Length");
                        return -1;
                    }
                    /* Two different lengths are a request-smuggling risk. */
                    if (out->has_length && out->content_length != cl) {
                        set_err(err, errcap, "conflicting Content-Length headers");
                        return -1;
                    }
                    out->content_length = cl;
                    out->has_length = 1;
                } else if (a98_strcasecmp(name, "Transfer-Encoding") == 0) {
                    size_t vlen = strlen(value);
                    size_t k;
                    for (k = 0; k + 7 <= vlen; k++) {
                        if (a98_strncasecmp(value + k, "chunked", 7) == 0) {
                            out->chunked = 1;
                            break;
                        }
                    }
                } else if (a98_strcasecmp(name, "Content-Type") == 0) {
                    strncpy(out->content_type, value, sizeof(out->content_type) - 1);
                    out->content_type[sizeof(out->content_type) - 1] = '\0';
                } else if (a98_strcasecmp(name, "Location") == 0) {
                    strncpy(out->location, value, sizeof(out->location) - 1);
                    out->location[sizeof(out->location) - 1] = '\0';
                } else if (a98_strcasecmp(name, "Connection") == 0) {
                    if (a98_strcasecmp(value, "close") == 0) {
                        out->connection_close = 1;
                    }
                }
            }
            line_no++;
            line_start = i + 1;
        }
    }
    if (out->status < 100 || out->status > 599) {
        set_err(err, errcap, "invalid HTTP status code");
        return -1;
    }
    return 0;
}

/* Is `name` already present in a "Name: value\r\n" block?  Used to avoid
 * sending two Host or Accept headers when the caller supplies its own. */
static int headers_contain(const char *block, const char *name)
{
    const char *p;
    size_t nlen;

    if (block == NULL || name == NULL) {
        return 0;
    }
    nlen = strlen(name);
    p = block;
    while (*p != '\0') {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\n') {
            eol++;
        }
        if ((size_t)(eol - p) > nlen &&
            a98_strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            return 1;
        }
        p = (*eol == '\0') ? eol : eol + 1;
    }
    return 0;
}

/* Validate caller-supplied "Name: value\r\n" lines before they are copied
 * into the request.  Header names must consist only of RFC 7230 tchar bytes;
 * values may not contain NUL/control bytes (HTAB is allowed as HTTP OWS) or a
 * bare CR.  Every line may end with CRLF (the last one may omit it). */
static int validate_extra_headers(const char *h, char *err, size_t errcap)
{
    const char *p;

    if (h == NULL) {
        return 0;
    }
    p = h;
    while (*p != '\0') {
        const char *colon = p;
        const char *q;
        const char *eol;

        while (*colon != '\0' && *colon != '\r' && *colon != '\n' &&
               *colon != ':') {
            colon++;
        }
        if (colon == p || *colon != ':') {
            set_err(err, errcap, "invalid extra header: missing or empty name");
            return -1;
        }
        for (q = p; q < colon; q++) {
            if (!is_tchar(*q)) {
                set_err(err, errcap,
                        "invalid extra header: illegal character in name");
                return -1;
            }
        }
        q = colon + 1;
        eol = q;
        while (*eol != '\0' && *eol != '\r' && *eol != '\n') {
            eol++;
        }
        for (; q < eol; q++) {
            unsigned char c = (unsigned char)*q;
            if (c == '\t') {
                continue;
            }
            if (c < 0x20 || c == 0x7F) {
                set_err(err, errcap,
                        "invalid extra header: control character in value");
                return -1;
            }
        }
        if (*eol != '\0') {
            const char *e = eol;

            /* Tolerate a run of CRs before the LF: one caller normalizes
             * lines by appending CRLF to input that already had one.  A CR
             * followed by ordinary text is still rejected. */
            while (*e == '\r') {
                e++;
            }
            if (*e == '\n') {
                p = e + 1;
            } else if (*e == '\0') {
                p = e;
            } else {
                set_err(err, errcap, "invalid extra header: bare CR");
                return -1;
            }
        } else {
            p = eol; /* final line without CRLF; build_request adds it */
        }
    }
    return 0;
}

static int build_request(Str *req, const char *method, const Url *url,
                         const char *extra_headers, size_t body_len,
                         char *err, size_t errcap)
{
    char host[300];
    size_t i;
    size_t method_len;

    if (method == NULL || url == NULL || req == NULL) {
        set_err(err, errcap, "invalid HTTP request arguments");
        return -1;
    }
    method_len = strlen(method);
    if (method_len == 0 || method_len > 32) {
        set_err(err, errcap, "invalid HTTP method");
        return -1;
    }
    for (i = 0; i < method_len; i++) {
        if (!is_tchar(method[i])) {
            set_err(err, errcap, "invalid HTTP method");
            return -1;
        }
    }
    if (validate_extra_headers(extra_headers, err, errcap) != 0) {
        return -1;
    }

    url_host_header(url, host, sizeof(host));
    if (str_appendf(req, "%s %s HTTP/1.1\r\n", method, url->path) != 0) {
        goto append_fail;
    }
    if (!headers_contain(extra_headers, "Host")) {
        if (str_appendf(req, "Host: %s\r\n", host) != 0) {
            goto append_fail;
        }
    }
    if (str_appendf(req, "User-Agent: AIagent98/%s\r\n", A98_VERSION) != 0) {
        goto append_fail;
    }
    if (!headers_contain(extra_headers, "Accept")) {
        if (str_appendf(req, "Accept: */*\r\n") != 0) {
            goto append_fail;
        }
    }
    if (str_appendf(req, "Connection: close\r\n") != 0) {
        goto append_fail;
    }
    if (extra_headers != NULL && extra_headers[0] != '\0') {
        if (str_append_cstr(req, extra_headers) != 0) {
            goto append_fail;
        }
        if (req->len >= 2 &&
            !(req->data[req->len - 2] == '\r' && req->data[req->len - 1] == '\n')) {
            if (str_append_cstr(req, "\r\n") != 0) {
                goto append_fail;
            }
        }
    }
    if (body_len > 0) {
        if (str_appendf(req, "Content-Length: %lu\r\n",
                        (unsigned long)body_len) != 0) {
            goto append_fail;
        }
    }
    if (str_append_cstr(req, "\r\n") != 0) {
        goto append_fail;
    }
    return 0;

append_fail:
    set_err(err, errcap, "out of memory building HTTP request");
    return -1;
}

int http_request(NetConn *conn, const char *method, const Url *url,
                 const char *extra_headers, const char *body, size_t body_len,
                 HttpResponse *resp, HttpSink sink, void *sink_ctx,
                 char *err, size_t errcap)
{
    Str req;
    Str head;
    char buf[4096];
    BodyDecoder decoder;
    int got_head = 0;
    int rc = -1;
    int read_err = 0;
    int saw_eof = 0;

    if (conn == NULL || url == NULL || method == NULL || resp == NULL) {
        set_err(err, errcap, "invalid HTTP request arguments");
        return -1;
    }
    str_init(&req);
    str_init(&head);
    memset(&decoder, 0, sizeof(decoder));
    str_init(&decoder.line);

    if (body == NULL) {
        body_len = 0;
    }

    if (build_request(&req, method, url, extra_headers, body_len,
                      err, errcap) != 0) {
        goto done;
    }
    log_debug("HTTP %s %s (%lu body bytes)", method, url->path, (unsigned long)body_len);
    if (net_write_all(conn, str_cstr(&req), req.len) != 0) {
        set_err(err, errcap, "%s", net_error(conn));
        goto done;
    }
    if (body_len > 0 && body != NULL) {
        if (net_write_all(conn, body, body_len) != 0) {
            set_err(err, errcap, "%s", net_error(conn));
            goto done;
        }
    }

    for (;;) {
        int n = net_read(conn, buf, sizeof(buf));
        if (n == 0) {
            saw_eof = 1;
            break;
        }
        if (n < 0) {
            read_err = 1;
            break;
        }
        if (!got_head) {
            if (str_append(&head, buf, (size_t)n) != 0) {
                set_err(err, errcap, "out of memory reading HTTP response");
                goto done;
            }
            if (head.len > 65536) {
                set_err(err, errcap, "HTTP response head too large");
                goto done;
            }
            if (http_parse_response_head(head.data, head.len, resp, err, errcap) == 0) {
                size_t body_off = resp->head_len;
                if (resp->status >= 100 && resp->status < 200) {
                    /* 1xx (e.g. 100 Continue) is informational and has no
                     * body.  Drop its head and parse the final response that
                     * follows, keeping any bytes already buffered. */
                    size_t rem = head.len - body_off;
                    log_debug("skipping informational HTTP response %d",
                              resp->status);
                    if (rem > 0) {
                        memmove(head.data, head.data + body_off, rem);
                    }
                    head.len = rem;
                    if (head.data != NULL) {
                        head.data[head.len] = '\0';
                    }
                    continue;
                }
                got_head = 1;
                if (resp->status == 204 || resp->status == 304 ||
                    a98_strcasecmp(method, "HEAD") == 0) {
                    body_decoder_init(&decoder, 0, 0, 0);
                } else if (resp->chunked) {
                    body_decoder_init(&decoder, 1, 0, 0);
                } else if (resp->has_length) {
                    body_decoder_init(&decoder, 0, resp->content_length, 0);
                } else {
                    body_decoder_init(&decoder, 0, 0, 1);
                }
                if (body_off < head.len) {
                    if (body_decoder_feed(&decoder, head.data + body_off,
                                          head.len - body_off, sink, sink_ctx,
                                          err, errcap) != 0) {
                        goto done;
                    }
                }
                str_free(&head);
                str_init(&head);
            } else {
                /* Parsing fails until the blank line arrives; only report
                 * real format errors once the head looks complete. */
                if (strstr(str_cstr(&head), "\r\n\r\n") != NULL ||
                    strstr(str_cstr(&head), "\n\n") != NULL) {
                    goto done;
                }
            }
        } else {
            if (body_decoder_feed(&decoder, buf, (size_t)n, sink, sink_ctx,
                                  err, errcap) != 0) {
                goto done;
            }
            if (decoder.done) {
                break;
            }
        }
        if (decoder.done) {
            break;
        }
    }

    if (!got_head) {
        if (read_err && net_error(conn)[0] != '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        } else {
            set_err(err, errcap, "server closed the connection before sending a response");
        }
        goto done;
    }
    if (decoder.mode == 2) {
        /* EOF-delimited body: only a clean EOF completes the body. */
        if (!saw_eof) {
            if (read_err && net_error(conn)[0] != '\0') {
                set_err(err, errcap, "%s", net_error(conn));
            } else {
                set_err(err, errcap, "connection closed before the HTTP body was complete");
            }
            goto done;
        }
        if (body_decoder_finish(&decoder, err, errcap) != 0) {
            goto done;
        }
    } else if (!decoder.done) {
        if (read_err && net_error(conn)[0] != '\0') {
            set_err(err, errcap, "%s", net_error(conn));
        } else if (body_decoder_finish(&decoder, err, errcap) != 0) {
            /* error already set */
        }
        goto done;
    }
    rc = 0;
    goto done;

done:
    body_decoder_free(&decoder);
    str_free(&req);
    str_free(&head);
    if (rc != 0 && err != NULL && errcap > 0 && err[0] == '\0') {
        set_err(err, errcap, "HTTP request failed");
    }
    return rc;
}
