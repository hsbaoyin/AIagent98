/*
 * AIagent98 - network tools.  All of them are gated by [net] allow=1 and an
 * optional host allow-list.  The heavy lifting (TLS 1.2, chunked HTTP, SSE)
 * is already in src/net; these tools just expose it to the model.
 */
#include "tools_net.h"
#include "agent.h"
#include "jsonutil.h"

#include "../core/log.h"
#include "../core/path.h"
#include "../core/utf8.h"
#include "../net/net.h"
#include "../net/http.h"
#include "../net/url.h"

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static const char *arg_str(cJSON *args, const char *name, const char *def)
{
    const char *v = json_string(args, name);
    return v != NULL ? v : def;
}

/* Match a host against the comma-separated [net] allow_hosts list. An empty
 * list allows any host; an entry may start with "." to match a suffix. */
static int host_allowed(const Config *cfg, const char *host)
{
    char list[512];
    char *p;
    size_t i;
    int found = 0;

    if (cfg->net_allow_hosts[0] == '\0') return 1;
    strncpy(list, cfg->net_allow_hosts, sizeof(list) - 1);
    list[sizeof(list) - 1] = '\0';
    p = list;
    while (*p != '\0') {
        char *e = p;
        char *start;
        while (*e != '\0' && *e != ',') e++;
        if (*e == ',') *e = '\0';
        start = p;
        while (*start == ' ' || *start == '\t') start++;
        if (*start != '\0') {
            if (a98_strcasecmp(start, host) == 0) { found = 1; break; }
            if (start[0] == '.') {
                size_t hl = strlen(host), sl = strlen(start);
                if (hl >= sl &&
                    a98_strcasecmp(host + hl - sl, start) == 0) { found = 1; break; }
            }
        }
        p = e + 1;
    }
    (void)i;
    return found;
}

static int net_gate(Agent *a, const char *host, char *err, size_t errcap)
{
    if (a == NULL || a->cfg == NULL) {
        set_err(err, errcap, "agent is not initialised");
        return -1;
    }
    if (!a->cfg->net_allow) {
        set_err(err, errcap,
                "network tools are disabled ([net] allow=1 required)");
        return -1;
    }
    if (!host_allowed(a->cfg, host)) {
        set_err(err, errcap,
                "host '%s' is not in [net] allow_hosts", host);
        return -1;
    }
    return 0;
}

/* ---- fetch helper ---------------------------------------------------- */

typedef struct {
    Str *body;
    size_t max;
    size_t total;
    int truncated;
} FetchCtx;

static int fetch_sink(void *ctx, const char *data, size_t len)
{
    FetchCtx *fc = (FetchCtx *)ctx;
    size_t room;

    if (fc->total >= fc->max) { fc->truncated = 1; return 0; }
    room = fc->max - fc->total;
    if (len > room) len = room;
    str_append(fc->body, data, len);
    fc->total += len;
    if (fc->total >= fc->max) fc->truncated = 1;
    return 0;
}

/* Fetch a URL into `out`. On success returns 0 and fills resp/total/truncated. */
static int net_fetch(Agent *a, const char *url, const char *method,
                     const char *headers, const char *body, size_t body_len,
                     Str *out, HttpResponse *resp, size_t *total,
                     int *truncated, char *err, size_t errcap)
{
    Url u;
    NetConn *conn;
    FetchCtx fc;
    int rc;

    if (url_parse(url, &u, err, errcap) != 0) return -1;
    if (net_gate(a, u.host, err, errcap) != 0) return -1;

    conn = net_open(&u, a->cfg->tls_verify,
                    a->cfg->connect_timeout_sec,
                    a->cfg->request_timeout_sec, err, errcap);
    if (conn == NULL) return -1;
    net_set_cancel(conn, a->cancel);

    fc.body = out;
    fc.max = (size_t)a->cfg->net_max_bytes;
    fc.total = 0;
    fc.truncated = 0;

    rc = http_request(conn, method, &u, headers, body, body_len,
                      resp, fetch_sink, &fc, err, errcap);
    net_close(conn);
    if (rc != 0) return -1;
    if (total != NULL) *total = fc.total;
    if (truncated != NULL) *truncated = fc.truncated;
    return 0;
}

/* ---- http_request ---------------------------------------------------- */

const char schema_http_request[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\",\"description\":\"http(s) URL to request.\"},"
    "\"method\":{\"type\":\"string\",\"description\":\"GET (default), HEAD, POST, PUT or DELETE.\"},"
    "\"headers\":{\"type\":\"string\",\"description\":\"Extra headers, one 'Name: value' per line.\"},"
    "\"body\":{\"type\":\"string\",\"description\":\"Request body (UTF-8).\"},"
    "\"out_file\":{\"type\":\"string\",\"description\":\"Optional workspace file to save the raw body to.\"},"
    "\"max_bytes\":{\"type\":\"integer\",\"description\":\"Override [net] max_bytes for this call.\"}},"
    "\"required\":[\"url\"]}";

int tool_http_request_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *url = arg_str(args, "url", NULL);
    const char *method = arg_str(args, "method", "GET");
    const char *headers = arg_str(args, "headers", NULL);
    const char *body = arg_str(args, "body", NULL);
    const char *out_file = arg_str(args, "out_file", NULL);
    int override = json_int(args, "max_bytes", 0);
    HttpResponse resp;
    Str raw;
    size_t total = 0;
    int truncated = 0;
    char path[A98_MAX_PATH];
    FILE *f;
    int save_raw = 0;
    int old_max;

    if (url == NULL || url[0] == '\0') {
        set_err(err, errcap, "url is required");
        return -1;
    }
    /* Ensure a trailing CRLF on each header line for build_request(). */
    {
        Str h;
        str_init(&h);
        if (headers != NULL && headers[0] != '\0') {
            const char *p = headers;
            while (*p != '\0') {
                const char *e = p;
                while (*e != '\0' && *e != '\n') e++;
                str_append(&h, p, (size_t)(e - p));
                str_append_cstr(&h, "\r\n");
                p = (*e == '\n') ? e + 1 : e;
            }
        }
        if (override > 0) {
            if (override < 1024) override = 1024;
            if (override > 8 * 1024 * 1024) override = 8 * 1024 * 1024;
            old_max = a->cfg->net_max_bytes;
            a->cfg->net_max_bytes = override;
        } else old_max = 0;

        str_init(&raw);
        memset(&resp, 0, sizeof(resp));
        {
            char m[16];
            size_t i;
            for (i = 0; method[i] != '\0' && i < sizeof(m) - 1; i++)
                m[i] = (char)toupper((unsigned char)method[i]);
            m[i] = '\0';
            if (net_fetch(a, url, m, str_cstr(&h), body,
                          body != NULL ? strlen(body) : 0,
                          &raw, &resp, &total, &truncated, err, errcap) != 0) {
                str_free(&h); str_free(&raw);
                if (old_max) a->cfg->net_max_bytes = old_max;
                return -1;
            }
        }
        if (old_max) a->cfg->net_max_bytes = old_max;
        str_free(&h);
    }

    if (out_file != NULL && out_file[0] != '\0') {
        if (tools_resolve_path(a, out_file, path, sizeof(path), err, errcap) != 0) {
            str_free(&raw);
            return -1;
        }
        f = fopen(path, "wb");
        if (f == NULL) {
            str_free(&raw);
            set_err(err, errcap, "cannot write out_file: %s", path);
            return -1;
        }
        fwrite(str_cstr(&raw), 1, str_cstr(&raw)[0] ? strlen(str_cstr(&raw)) : 0, f);
        fclose(f);
        save_raw = 1;
    }

    str_appendf(out, "HTTP %d %s\n", resp.status, resp.reason);
    if (resp.content_type[0] != '\0')
        str_appendf(out, "content-type: %s\n", resp.content_type);
    str_appendf(out, "bytes: %lu%s\n", (unsigned long)total,
                truncated ? " (truncated)" : "");
    if (save_raw)
        str_appendf(out, "saved to: %s\n", out_file);
    if (!save_raw && raw.len > 0) {
        char *clean = a98_utf8_sanitize(str_cstr(&raw));
        str_append_cstr(out, clean != NULL ? clean : str_cstr(&raw));
        a98_free(clean);
        if (out->len > 0 && out->data[out->len - 1] != '\n')
            str_append_char(out, '\n');
    }
    str_free(&raw);
    return 0;
}
const ToolDef tool_http_request = {
    "http_request",
    "Make an HTTP(S) request (GET/POST/etc). Disabled unless [net] allow=1.",
    schema_http_request, 0, tool_http_request_run
};

/* ---- http_download --------------------------------------------------- */

const char schema_http_download[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"url\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Workspace file to write.\"},"
    "\"max_bytes\":{\"type\":\"integer\"}},"
    "\"required\":[\"url\",\"path\"]}";

int tool_http_download_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *url = arg_str(args, "url", NULL);
    const char *user = arg_str(args, "path", NULL);
    int override = json_int(args, "max_bytes", 0);
    char path[A98_MAX_PATH];
    HttpResponse resp;
    Str raw;
    size_t total = 0;
    int truncated = 0;
    int old_max;
    FILE *f;

    if (url == NULL || url[0] == '\0' || user == NULL || user[0] == '\0') {
        set_err(err, errcap, "url and path are required");
        return -1;
    }
    if (tools_resolve_path(a, user, path, sizeof(path), err, errcap) != 0)
        return -1;
    if (override > 0) {
        if (override < 1024) override = 1024;
        if (override > 8 * 1024 * 1024) override = 8 * 1024 * 1024;
        old_max = a->cfg->net_max_bytes;
        a->cfg->net_max_bytes = override;
    } else old_max = 0;

    str_init(&raw);
    memset(&resp, 0, sizeof(resp));
    if (net_fetch(a, url, "GET", NULL, NULL, 0, &raw, &resp,
                  &total, &truncated, err, errcap) != 0) {
        str_free(&raw);
        if (old_max) a->cfg->net_max_bytes = old_max;
        return -1;
    }
    if (old_max) a->cfg->net_max_bytes = old_max;
    if (resp.status < 200 || resp.status >= 300) {
        str_free(&raw);
        set_err(err, errcap, "download failed: HTTP %d %s", resp.status, resp.reason);
        return -1;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        str_free(&raw);
        set_err(err, errcap, "cannot write file: %s", path);
        return -1;
    }
    fwrite(str_cstr(&raw), 1, strlen(str_cstr(&raw)), f);
    fclose(f);
    str_appendf(out, "downloaded %lu bytes to %s\n", (unsigned long)total, user);
    if (truncated) str_append_cstr(out, "WARNING: response was truncated\n");
    str_free(&raw);
    return 0;
}
const ToolDef tool_http_download = {
    "http_download", "Download a URL to a workspace file. Disabled unless [net] allow=1.",
    schema_http_download, 0, tool_http_download_run
};

/* ---- dns_resolve ----------------------------------------------------- */

const char schema_dns_resolve[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"host\":{\"type\":\"string\"}},"
    "\"required\":[\"host\"]}";

int tool_dns_resolve_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *host = arg_str(args, "host", NULL);
    struct hostent *he;
    int i;

    if (host == NULL || host[0] == '\0') {
        set_err(err, errcap, "host is required");
        return -1;
    }
    if (net_gate(a, host, err, errcap) != 0) return -1;
    he = gethostbyname(host);
    if (he == NULL) {
        set_err(err, errcap, "cannot resolve '%s' (WSA %d)", host,
                WSAGetLastError());
        return -1;
    }
    str_appendf(out, "%s -> %s\n", host, he->h_name);
    for (i = 0; he->h_addr_list[i] != NULL; i++) {
        struct in_addr ia;
        memcpy(&ia, he->h_addr_list[i], sizeof(ia));
        str_appendf(out, "  %s\n", inet_ntoa(ia));
    }
    return 0;
}
const ToolDef tool_dns_resolve = {
    "dns_resolve", "Resolve a hostname to IP addresses. Disabled unless [net] allow=1.",
    schema_dns_resolve, 0, tool_dns_resolve_run
};

/* ---- tcp_probe ------------------------------------------------------- */

const char schema_tcp_probe[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"host\":{\"type\":\"string\"},"
    "\"port\":{\"type\":\"integer\"},"
    "\"timeout_sec\":{\"type\":\"integer\"}},"
    "\"required\":[\"host\",\"port\"]}";

int tool_tcp_probe_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *host = arg_str(args, "host", NULL);
    int port = json_int(args, "port", 0);
    int timeout = json_int(args, "timeout_sec", 5);
    struct hostent *he;
    struct sockaddr_in addr;
    SOCKET s;
    u_long nonblock = 1;
    fd_set wf;
    struct timeval tv;
    int rc, soerr;
    int soerr_len;

    if (host == NULL || host[0] == '\0' || port < 1 || port > 65535) {
        set_err(err, errcap, "host and a valid port are required");
        return -1;
    }
    if (net_gate(a, host, err, errcap) != 0) return -1;
    if (timeout < 1) timeout = 1;
    if (timeout > 60) timeout = 60;

    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
        set_err(err, errcap, "cannot resolve '%s'", host);
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        set_err(err, errcap, "socket failed (WSA %d)", WSAGetLastError());
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
    ioctlsocket(s, FIONBIO, &nonblock);
    rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        str_appendf(out, "%s:%d is reachable\n", host, port);
        closesocket(s);
        return 0;
    }
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    rc = select(0, NULL, &wf, NULL, &tv);
    soerr = 0;
    soerr_len = sizeof(soerr);
    if (rc > 0)
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &soerr_len);
    closesocket(s);
    if (rc > 0 && soerr == 0) {
        str_appendf(out, "%s:%d is reachable\n", host, port);
        return 0;
    }
    str_appendf(out, "%s:%d is NOT reachable (WSA %d%s)\n", host, port,
                rc > 0 ? soerr : WSAGetLastError(),
                rc == 0 ? " (timeout)" : "");
    return 0;
}
const ToolDef tool_tcp_probe = {
    "tcp_probe", "Test whether a host:port is reachable. Disabled unless [net] allow=1.",
    schema_tcp_probe, 0, tool_tcp_probe_run
};

/* ---- web_search ------------------------------------------------------ */

static void url_encode(const char *s, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;

    while (*p != '\0' && o + 3 < cap) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else if (*p == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
        p++;
    }
    out[o] = '\0';
}

static void html_unescape(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    while (*s != '\0' && o + 1 < cap) {
        if (*s == '&') {
            if (strncmp(s, "&amp;", 5) == 0) { out[o++] = '&'; s += 5; continue; }
            if (strncmp(s, "&lt;", 4) == 0) { out[o++] = '<'; s += 4; continue; }
            if (strncmp(s, "&gt;", 4) == 0) { out[o++] = '>'; s += 4; continue; }
            if (strncmp(s, "&quot;", 6) == 0) { out[o++] = '"'; s += 6; continue; }
            if (strncmp(s, "&#39;", 5) == 0) { out[o++] = '\''; s += 5; continue; }
        }
        out[o++] = *s++;
    }
    out[o] = '\0';
}

/* Extract text between the current position and the next '<'. */
static void html_tag_text(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    while (*p != '\0' && *p != '<' && o + 1 < cap) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    html_unescape(out, out, cap);
}

const char schema_web_search[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\"},"
    "\"max_results\":{\"type\":\"integer\"}},"
    "\"required\":[\"query\"]}";

/* Built-in search engines. A primary template from [net] search_url is tried
 * first, then these. Having several makes web_search work both in China
 * (Bing/Baidu reachable) and abroad (DuckDuckGo reachable). */
typedef struct {
    const char *url;
    const char *name;
} SearchEngine;

static const SearchEngine g_search_engines[] = {
    { "https://www.bing.com/search?q=%s",       "bing" },
    { "https://cn.bing.com/search?q=%s",        "bing-cn" },
    { "https://www.baidu.com/s?wd=%s",          "baidu" },
    { "https://html.duckduckgo.com/html/?q=%s", "duckduckgo" },
    { NULL, NULL }
};

/* Extract the href="..." attribute that appears before `tag` within the same
 * opening <a ...> element. Returns 1 on success. */
static int find_href(const char *html, const char *marker,
                     char *href, size_t cap)
{
    const char *h = marker;
    const char *tag_end;
    size_t n = 0;
    char decoded[A98_MAX_PATH];

    /* The marker may be inside the <a> (DDG: class="result__a") or on an
     * outer element (Bing: <h2>, Baidu: <h3>). Search forward from the
     * marker for the next href="..." within a bounded window. */
    tag_end = marker + 512;
    h = strstr(marker, "href=\"");
    if (h == NULL || h > tag_end) return 0;
    h += 6;
    while (h[n] != '\0' && h[n] != '"' && n < cap - 1) n++;
    href[n] = '\0';
    memcpy(href, h, n);
    /* decode &amp; etc; entities only contract so same-size buffer is safe */
    html_unescape(href, decoded, sizeof(decoded));
    strncpy(href, decoded, cap - 1);
    href[cap - 1] = '\0';
    return 1;
}

/* Find the text following the `>` that closes the tag at `p`. */
static int tag_text(const char *p, char *out, size_t cap)
{
    const char *te = strchr(p, '>');
    if (te == NULL) return 0;
    html_tag_text(te + 1, out, cap);
    return out[0] != '\0';
}

/* One parsed result. */
typedef struct {
    char title[512];
    char href[1024];
    char snippet[1024];
} SearchResult;

/* Format markers for the supported engines. The parser looks for a "block"
 * marker (one result) and a "link" marker (the <a> carrying title + href). */
typedef struct {
    const char *block;   /* marks the start of one result */
    const char *link;    /* marks the <a> inside a result */
    const char *snippet; /* optional snippet marker */
} HtmlFormat;

static const HtmlFormat g_formats[] = {
    /* DuckDuckGo HTML */
    { "class=\"result__a\"",   "class=\"result__a\"",   "class=\"result__snippet\"" },
    /* Bing */
    { "class=\"b_algo\"",      "<h2>",                   "class=\"b_caption\"" },
    /* Baidu */
    { "class=\"c-container\"", "<h3",                    "class=\"c-abstract\"" },
    { NULL, NULL, NULL }
};

/* Parse results from `html` into `out`. Returns the number found. */
static int parse_search_html(const char *html, Str *out, int max_results)
{
    int i, count = 0;
    const HtmlFormat *fmt = NULL;

    for (i = 0; g_formats[i].block != NULL; i++) {
        if (strstr(html, g_formats[i].block) != NULL) {
            fmt = &g_formats[i];
            break;
        }
    }
    if (fmt == NULL) return 0;

    {
        const char *p = html;
        while (*p != '\0' && count < max_results) {
            const char *block = strstr(p, fmt->block);
            SearchResult r;
            if (block == NULL) break;
            {
                const char *link = strstr(block, fmt->link);
                const char *anchor;
                if (link == NULL || !find_href(html, link, r.href, sizeof(r.href))) {
                    const char *adv = block + strlen(fmt->block);
                    p = adv > p ? adv : p + 1;
                    continue;
                }
                /* Locate the <a ...> element for the title: for DDG the
                 * marker is inside the <a>; for Bing/Baidu the marker is an
                 * outer <h2>/<h3> and the <a> follows. */
                anchor = link;
                {
                    const char *back = link;
                    while (back > html && *(back - 1) != '<') back--;
                    /* back points at the first char after '<'; step back
                     * onto the '<' itself before testing the tag name. */
                    if (back > html && strncmp(back - 1, "<a", 2) == 0) {
                        anchor = back - 1;        /* marker inside an <a> */
                    } else {
                        const char *a = strstr(link, "<a ");
                        if (a == NULL) a = strstr(link, "<a>");
                        if (a != NULL) anchor = a;
                    }
                }
                r.title[0] = '\0';
                tag_text(anchor, r.title, sizeof(r.title));
                if (r.title[0] == '\0') tag_text(link, r.title, sizeof(r.title));
                r.snippet[0] = '\0';
                if (fmt->snippet != NULL) {
                    const char *sn = strstr(block, fmt->snippet);
                    if (sn != NULL) tag_text(sn, r.snippet, sizeof(r.snippet));
                }
                if (count > 0) str_append_char(out, '\n');
                str_appendf(out, "%d. %s\n   %s\n", count + 1,
                            r.title[0] ? r.title : r.href, r.href);
                if (r.snippet[0] != '\0')
                    str_appendf(out, "   %s\n", r.snippet);
                count++;
            }
            {
                const char *adv = block + strlen(fmt->block);
                p = adv > block ? adv : block + 1;
            }
        }
    }
    return count;
}

/* Fetch one engine and parse. Returns 0 on success with results, -1 on
 * transport/HTTP error, or -2 on success but no parseable results. */
static int web_search_engine(Agent *a, const char *fmt, const char *enc,
                             const char *engine_name, Str *out,
                             char *err, size_t errcap)
{
    char url[1024];
    HttpResponse resp;
    Str html;
    int count;
    int old_timeout;

    if (snprintf(url, sizeof(url), fmt, enc) >= (int)sizeof(url)) {
        set_err(err, errcap, "search URL is too long");
        return -1;
    }
    /* Search pages are slow in some regions; keep it short so fallback can
     * kick in quickly. Restore the configured timeout afterwards. */
    old_timeout = a->cfg->request_timeout_sec;
    if (old_timeout > 15) a->cfg->request_timeout_sec = 15;

    str_init(&html);
    memset(&resp, 0, sizeof(resp));
    if (net_fetch(a, url, "GET", NULL, NULL, 0, &html, &resp,
                  NULL, NULL, err, errcap) != 0) {
        str_free(&html);
        a->cfg->request_timeout_sec = old_timeout;
        return -1;
    }
    a->cfg->request_timeout_sec = old_timeout;
    if (resp.status < 200 || resp.status >= 300) {
        str_free(&html);
        set_err(err, errcap, "search engine %s: HTTP %d",
                engine_name, resp.status);
        return -1;
    }
    count = parse_search_html(str_cstr(&html), out, 10);
    str_free(&html);
    if (count == 0) {
        set_err(err, errcap, "search engine %s: no parseable results",
                engine_name);
        return -2;
    }
    str_appendf(out, "[via %s]\n", engine_name);
    return 0;
}

int tool_web_search_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *query = arg_str(args, "query", NULL);
    char enc[512];
    Str attempt;
    char last_err[512];
    int i, rc = -1;
    int max_results;

    if (query == NULL || query[0] == '\0') {
        set_err(err, errcap, "query is required");
        return -1;
    }
    max_results = json_int(args, "max_results", 5);
    if (max_results < 1) max_results = 1;
    if (max_results > 10) max_results = 10;
    (void)max_results; /* parse_search_html caps at 10; caller can trim later */
    url_encode(query, enc, sizeof(enc));

    last_err[0] = '\0';
    /* 1) user-configured primary engine */
    if (a->cfg->net_search_url[0] != '\0') {
        str_init(&attempt);
        rc = web_search_engine(a, a->cfg->net_search_url, enc,
                               "custom", &attempt, last_err, sizeof(last_err));
        if (rc == 0) {
            str_append(out, str_cstr(&attempt), strlen(str_cstr(&attempt)));
            str_free(&attempt);
            return 0;
        }
        str_free(&attempt);
    }
    /* 2) built-in engines: first one that returns results wins */
    for (i = 0; g_search_engines[i].url != NULL; i++) {
        str_init(&attempt);
        rc = web_search_engine(a, g_search_engines[i].url, enc,
                               g_search_engines[i].name, &attempt,
                               last_err, sizeof(last_err));
        if (rc == 0) {
            str_append(out, str_cstr(&attempt), strlen(str_cstr(&attempt)));
            str_free(&attempt);
            return 0;
        }
        str_free(&attempt);
    }
    set_err(err, errcap, "all search engines failed; last: %s", last_err);
    return -1;
}

const ToolDef tool_web_search = {
    "web_search",
    "Search the web for documentation or facts. Disabled unless [net] allow=1.",
    schema_web_search, 0, tool_web_search_run
};

/* ---- gist_upload ----------------------------------------------------- */

const char schema_gist_upload[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Workspace file to upload.\"},"
    "\"description\":{\"type\":\"string\"},"
    "\"public\":{\"type\":\"boolean\"}},"
    "\"required\":[\"path\"]}";

int tool_gist_upload_run(Agent *a, cJSON *args, Str *out, char *err, size_t errcap)
{
    const char *user = arg_str(args, "path", NULL);
    const char *desc = arg_str(args, "description", "Uploaded from AIagent98");
    int is_public = json_bool(args, "public", 0);
    char path[A98_MAX_PATH];
    char fname[A98_MAX_PATH];
    char *raw = NULL;
    char *text = NULL;
    long size;
    FILE *f;
    cJSON *root, *files, *fileobj;
    char *body;
    Str headers;
    HttpResponse resp;
    Str response;
    cJSON *parsed;
    cJSON *url_node;
    int rc = -1;

    if (user == NULL || user[0] == '\0') {
        set_err(err, errcap, "path is required");
        return -1;
    }
    if (a->cfg->net_github_token[0] == '\0') {
        set_err(err, errcap, "gist upload needs [net] github_token");
        return -1;
    }
    if (tools_resolve_path(a, user, path, sizeof(path), err, errcap) != 0)
        return -1;
    if (net_gate(a, "api.github.com", err, errcap) != 0) return -1;

    f = fopen(path, "rb");
    if (f == NULL) { set_err(err, errcap, "cannot open file: %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size < 0 || size > 1024 * 1024) {
        fclose(f);
        set_err(err, errcap, "file is too large for a gist (max 1 MB)");
        return -1;
    }
    raw = (char *)a98_alloc((size_t)size + 1);
    if (raw == NULL) { fclose(f); set_err(err, errcap, "oom"); return -1; }
    size = (long)fread(raw, 1, (size_t)size, f);
    raw[size] = '\0';
    fclose(f);

    if (a98_utf8_valid(raw, (size_t)size)) {
        text = raw;
    } else {
        text = a98_acp_to_utf8(raw);
        if (text == NULL) { a98_free(raw); set_err(err, errcap, "cannot convert file to UTF-8"); return -1; }
    }
    /* file name = last path component */
    {
        char *slash = strrchr(path, '\\');
        if (slash == NULL) slash = strrchr(path, '/');
        strcpy(fname, slash != NULL ? slash + 1 : path);
    }

    root = cJSON_CreateObject();
    files = cJSON_CreateObject();
    fileobj = cJSON_CreateObject();
    if (root == NULL || files == NULL || fileobj == NULL) {
        set_err(err, errcap, "oom");
        goto done;
    }
    cJSON_AddStringToObject(root, "description", desc);
    cJSON_AddBoolToObject(root, "public", is_public ? 1 : 0);
    cJSON_AddStringToObject(fileobj, "content", text);
    cJSON_AddItemToObject(files, fname, fileobj);
    cJSON_AddItemToObject(root, "files", files);
    body = cJSON_PrintUnformatted(root);
    if (body == NULL) { set_err(err, errcap, "oom"); goto done; }

    str_init(&headers);
    str_appendf(&headers,
        "Accept: application/vnd.github+json\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "User-Agent: AIagent98/%s\r\n"
        "Authorization: Bearer %s\r\n",
        A98_VERSION, a->cfg->net_github_token);

    str_init(&response);
    memset(&resp, 0, sizeof(resp));
    if (net_fetch(a, "https://api.github.com/gists", "POST",
                  str_cstr(&headers), body, strlen(body),
                  &response, &resp, NULL, NULL, err, errcap) != 0) {
        str_free(&headers); str_free(&response); free(body);
        goto done;
    }
    str_free(&headers);
    free(body);
    if (resp.status < 200 || resp.status >= 300) {
        str_appendf(out, "GitHub returned HTTP %d %s\n", resp.status, resp.reason);
        {
            char *clean = a98_utf8_sanitize(str_cstr(&response));
            str_append_cstr(out, clean != NULL ? clean : str_cstr(&response));
            a98_free(clean);
        }
        str_free(&response);
        rc = -1;
        goto done;
    }
    parsed = cJSON_Parse(str_cstr(&response));
    url_node = parsed != NULL ? cJSON_GetObjectItem(parsed, "html_url") : NULL;
    if (cJSON_IsString(url_node)) {
        str_appendf(out, "gist created: %s\n", url_node->valuestring);
        rc = 0;
    } else {
        str_appendf(out, "gist created but no html_url in response\n");
        rc = 0;
    }
    if (parsed != NULL) cJSON_Delete(parsed);
    str_free(&response);

done:
    if (text != raw) a98_free(text);
    a98_free(raw);
    if (root != NULL) cJSON_Delete(root);
    return rc;
}
const ToolDef tool_gist_upload = {
    "gist_upload", "Upload a workspace file to a GitHub Gist. Needs [net] github_token and allow=1. Requires approval.",
    schema_gist_upload, 1, tool_gist_upload_run
};





