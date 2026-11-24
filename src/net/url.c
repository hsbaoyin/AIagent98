#include "url.h"

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        strncpy(err, msg, cap - 1);
        err[cap - 1] = '\0';
    }
}

/* Host and path go straight into request lines and header values (Host:),
 * so bytes outside printable ASCII would let an INI-supplied URL inject
 * extra headers or a bogus request line.  Accept only 0x21..0x7E. */
static int has_unprintable(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7E) {
            return 1;
        }
    }
    return 0;
}

int url_parse(const char *url, Url *out, char *err, size_t errcap)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path;
    size_t host_len;
    int port = 0;
    int tls;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (url == NULL || url[0] == '\0') {
        set_err(err, errcap, "empty URL");
        return -1;
    }
    if (a98_strncasecmp(url, "https://", 8) == 0) {
        tls = 1;
        p = url + 8;
        port = 443;
    } else if (a98_strncasecmp(url, "http://", 7) == 0) {
        tls = 0;
        p = url + 7;
        port = 80;
    } else {
        set_err(err, errcap, "URL must start with http:// or https://");
        return -1;
    }
    /* Only IPv4 literals/hostnames are supported: the parser splits on ':'
     * and the TLS layer resolves via gethostbyname(AF_INET).  Fail clearly
     * instead of mis-parsing "[::1]" as host "[" plus a bogus port. */
    if (*p == '[') {
        set_err(err, errcap, "IPv6 URLs are not supported");
        return -1;
    }
    host_start = p;
    while (*p != '\0' && *p != '/' && *p != '?' && *p != '#' && *p != ':') {
        if (*p == '@') {
            set_err(err, errcap, "userinfo in URLs is not supported");
            return -1;
        }
        if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E) {
            set_err(err, errcap, "invalid character in host");
            return -1;
        }
        p++;
    }
    host_end = p;
    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        set_err(err, errcap, "invalid host name");
        return -1;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') {
            set_err(err, errcap, "invalid port");
            return -1;
        }
        port = 0;
        while (*p >= '0' && *p <= '9') {
            int digit = *p - '0';

            /* Check before multiplying: a long decimal port can overflow a
             * signed int before the old post-multiply range check runs. */
            if (port > (65535 - digit) / 10) {
                set_err(err, errcap, "port out of range");
                return -1;
            }
            port = port * 10 + digit;
            p++;
        }
    } else if (*p == '/' || *p == '?' || *p == '#' || *p == '\0') {
        /* keep default port */
    } else {
        set_err(err, errcap, "invalid character in host");
        return -1;
    }
    if (port <= 0 || port > 65535) {
        set_err(err, errcap, "invalid port");
        return -1;
    }

    path = p;
    if (*path == '\0' || *path == '#') {
        strcpy(out->path, "/");
    } else if (*path == '?') {
        size_t n = strlen(path);
        if (has_unprintable(path, n)) {
            set_err(err, errcap, "invalid character in URL query");
            return -1;
        }
        /* Silent truncation would corrupt the query string: report it. */
        if (n + 2 > sizeof(out->path)) {
            set_err(err, errcap, "URL query too long");
            return -1;
        }
        out->path[0] = '/';
        memcpy(out->path + 1, path, n);
        out->path[n + 1] = '\0';
    } else {
        size_t n = strlen(path);
        const char *hash = strchr(path, '#');
        if (hash != NULL) {
            n = (size_t)(hash - path);
        }
        if (n + 1 > sizeof(out->path)) {
            set_err(err, errcap, "URL path too long");
            return -1;
        }
        if (has_unprintable(path, n)) {
            set_err(err, errcap, "invalid character in URL path");
            return -1;
        }
        memcpy(out->path, path, n);
        out->path[n] = '\0';
    }
    out->tls = tls;
    out->port = port;
    return 0;
}

void url_host_header(const Url *u, char *out, size_t cap)
{
    int default_port;

    if (u == NULL || out == NULL || cap == 0) {
        return;
    }
    default_port = u->tls ? 443 : 80;
    if (u->port == default_port) {
        strncpy(out, u->host, cap - 1);
        out[cap - 1] = '\0';
    } else {
        snprintf(out, cap, "%s:%d", u->host, u->port);
    }
}
