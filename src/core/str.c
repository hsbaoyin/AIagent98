#include "str.h"

void str_init(Str *s)
{
    if (s == NULL) {
        return;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_free(Str *s)
{
    if (s == NULL) {
        return;
    }
    a98_free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void str_clear(Str *s)
{
    if (s == NULL) {
        return;
    }
    s->len = 0;
    if (s->data != NULL) {
        s->data[0] = '\0';
    }
}

int str_reserve(Str *s, size_t extra)
{
    size_t need;
    size_t cap;
    char *p;

    if (s == NULL) {
        return -1;
    }
    need = s->len + extra + 1;
    if (need <= s->cap) {
        return 0;
    }
    cap = s->cap ? s->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = (char *)a98_realloc(s->data, cap);
    if (p == NULL) {
        return -1;
    }
    s->data = p;
    s->cap = cap;
    if (s->len == 0) {
        s->data[0] = '\0';
    }
    return 0;
}

int str_append(Str *s, const void *data, size_t len)
{
    if (s == NULL) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (str_reserve(s, len) != 0) {
        return -1;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    s->data[s->len] = '\0';
    return 0;
}

int str_append_cstr(Str *s, const char *text)
{
    if (text == NULL) {
        return 0;
    }
    return str_append(s, text, strlen(text));
}

int str_append_char(Str *s, char c)
{
    return str_append(s, &c, 1);
}

int str_appendf(Str *s, const char *fmt, ...)
{
    char small[256];
    char *buf = small;
    int rc = -1;
    size_t size = sizeof(small);
    int n;
    va_list ap;

    if (s == NULL || fmt == NULL) {
        return -1;
    }
    /* Same doubling retry as a98_strfmt(): on Win9x's CRT vsnprintf() a
     * truncated result comes back as -1, not as the required byte count.
     * Reformatting from a strlen() of the clipped text could never grow the
     * buffer and, worse, appended uninitialised bytes to the Str. */
    for (;;) {
        buf[size - 1] = '\0';
        va_start(ap, fmt);
        n = vsnprintf(buf, size, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < size) {
            rc = str_append(s, buf, (size_t)n);
            break;
        }
        if (n < 0 && strlen(buf) + 1 < size) {
            break;                 /* encoding error, not truncation */
        }
        if (size > (size_t)-1 / 2) {
            break;
        }
        size *= 2;
        if (buf == small) {
            buf = (char *)a98_alloc(size);
        } else {
            char *bigger = (char *)a98_realloc(buf, size);
            if (bigger == NULL) {
                a98_free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
        }
        if (buf == NULL) {
            break;
        }
    }
    if (buf != small) {
        a98_free(buf);
    }
    return rc;
}

int str_set(Str *s, const char *text)
{
    if (s == NULL) {
        return -1;
    }
    str_clear(s);
    return str_append_cstr(s, text);
}

char *str_detach(Str *s)
{
    char *p;

    if (s == NULL) {
        return NULL;
    }
    if (s->data == NULL) {
        p = a98_strdup("");
    } else {
        p = s->data;
    }
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return p;
}

const char *str_cstr(const Str *s)
{
    if (s == NULL || s->data == NULL) {
        return "";
    }
    return s->data;
}

void str_trim_eol(Str *s)
{
    if (s == NULL || s->data == NULL) {
        return;
    }
    while (s->len > 0) {
        char c = s->data[s->len - 1];
        if (c == '\r' || c == '\n') {
            s->len--;
        } else {
            break;
        }
    }
    s->data[s->len] = '\0';
}
