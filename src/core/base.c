#include "base.h"

void *a98_alloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return malloc(n);
}

void *a98_realloc(void *p, size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return realloc(p, n);
}

void a98_free(void *p)
{
    if (p != NULL) {
        free(p);
    }
}

char *a98_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return a98_strndup(s, strlen(s));
}

char *a98_strndup(const char *s, size_t n)
{
    char *p;
    size_t len = 0;

    if (s == NULL) {
        return NULL;
    }
    while (len < n && s[len] != '\0') {
        len++;
    }
    p = (char *)a98_alloc(len + 1);
    if (p == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(p, s, len);
    }
    p[len] = '\0';
    return p;
}

/* ASCII-only folding, deliberately byte oriented: this helper is also used
 * on Windows 9x where text may be GBK/CP936.  A GBK trail byte may happen to
 * fall in 0x41..0x5A ('A'..'Z'); folding such a byte on its own would change
 * the character.  Bytes >= 0x80 are therefore treated as opaque lead bytes
 * and compared verbatim together with the byte that follows them (the same
 * rule is harmless for UTF-8, whose continuation bytes are never A..Z). */
static int fold_byte(int c, int *skip)
{
    if (*skip) {
        *skip = 0;                 /* trail/continuation byte: keep as-is */
        return c;
    }
    if (c >= 0x80) {
        *skip = 1;                 /* lead byte: next byte is part of it */
        return c;
    }
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

int a98_strcasecmp(const char *a, const char *b)
{
    int skip_a = 0;
    int skip_b = 0;

    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    for (;;) {
        int ca = fold_byte((unsigned char)*a, &skip_a);
        int cb = fold_byte((unsigned char)*b, &skip_b);
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
        a++;
        b++;
    }
}

int a98_strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;
    int skip_a = 0;
    int skip_b = 0;

    if (a == NULL) {
        a = "";
    }
    if (b == NULL) {
        b = "";
    }
    for (i = 0; i < n; i++) {
        int ca = fold_byte((unsigned char)a[i], &skip_a);
        int cb = fold_byte((unsigned char)b[i], &skip_b);
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
    return 0;
}

char *a98_strfmt(const char *fmt, ...)
{
    char small[256];
    char *buf = small;
    char *out = NULL;
    size_t size = sizeof(small);
    int n;
    va_list ap;

    if (fmt == NULL) {
        return NULL;
    }
    for (;;) {
        /* Pre-terminate: old MSVCRT vsnprintf() reports truncation as -1
         * instead of the would-be length, and its output is NUL-terminated. */
        buf[size - 1] = '\0';
        va_start(ap, fmt);
        n = vsnprintf(buf, size, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < size) {
            out = a98_strndup(buf, (size_t)n);
            break;
        }
        /* n < 0 means either "truncated" (pre-C99 CRT) or an encoding error.
         * A short/empty buffer in the latter case is a real failure; a full
         * one is truncation, so double and retry (str_appendf does the same). */
        if (n < 0 && strlen(buf) + 1 < size) {
            break;
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
    return out;
}
