#include "utf8.h"
#include "str.h"

#include <limits.h>

#ifdef _WIN32
#include <windows.h>
/* MultiByteToWideChar/WideCharToMultiByte take wchar_t*, while this module's
 * UTF-16 codec uses 16-bit units throughout.  Lock that ABI assumption down:
 * on a 32-bit-wchar_t build the casts below would corrupt the heap.  OpenWatcom
 * has no C_ASSERT, so use the standard negative-array typedef trick. */
typedef char a98_wchar_t_must_be_2_bytes[(sizeof(wchar_t) == 2) ? 1 : -1];
#endif

int a98_utf8_decode(const char *s, size_t len, unsigned int *cp)
{
    unsigned char c;
    unsigned int v;
    int need;
    int i;

    if (s == NULL || len == 0) {
        return -1;
    }
    c = (unsigned char)s[0];
    if (c < 0x80) {
        if (cp != NULL) {
            *cp = c;
        }
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        v = c & 0x1F;
        need = 1;
    } else if ((c & 0xF0) == 0xE0) {
        v = c & 0x0F;
        need = 2;
    } else if ((c & 0xF8) == 0xF0) {
        v = c & 0x07;
        need = 3;
    } else {
        return -1;
    }
    if (len < (size_t)(need + 1)) {
        return -1;
    }
    for (i = 1; i <= need; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) {
            return -1;
        }
        v = (v << 6) | (unsigned int)(cc & 0x3F);
    }
    /* Reject overlong encodings, surrogates and out-of-range values. */
    if ((need == 1 && v < 0x80) ||
        (need == 2 && v < 0x800) ||
        (need == 3 && v < 0x10000) ||
        v > 0x10FFFF ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        return -1;
    }
    if (cp != NULL) {
        *cp = v;
    }
    return need + 1;
}

int a98_utf8_encode(unsigned int cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return -1;
        }
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return -1;
}

int a98_utf8_valid(const char *s, size_t len)
{
    size_t i = 0;
    int n;

    if (s == NULL) {
        return 0;
    }
    while (i < len) {
        n = a98_utf8_decode(s + i, len - i, NULL);
        if (n <= 0) {
            return 0;
        }
        i += (size_t)n;
    }
    return 1;
}

unsigned short *a98_utf8_to_utf16(const char *s, size_t *out_units)
{
    size_t cap;
    size_t len;
    size_t i;
    size_t o;
    unsigned short *w;
    int n;
    unsigned int cp;

    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    if (len > (size_t)-1 / sizeof(unsigned short) - 2) {
        return NULL;
    }
    cap = len + 1;
    w = (unsigned short *)a98_alloc((cap + 1) * sizeof(unsigned short));
    if (w == NULL) {
        return NULL;
    }
    i = 0;
    o = 0;
    while (i < len) {
        if (o + 3 > cap) {
            unsigned short *bigger;
            /* Guard both the doubling and the element-count multiplication
             * before realloc(); a wrapped size used to allocate a tiny
             * buffer and then write far past it. */
            if (cap > ((size_t)-1 - 16) / 2 ||
                cap >= (size_t)-1 / sizeof(unsigned short) - 16) {
                a98_free(w);
                return NULL;
            }
            cap = cap * 2 + 16;
            bigger = (unsigned short *)a98_realloc(w, (cap + 1) * sizeof(unsigned short));
            if (bigger == NULL) {
                a98_free(w);
                return NULL;
            }
            w = bigger;
        }
        n = a98_utf8_decode(s + i, len - i, &cp);
        if (n <= 0) {
            w[o++] = (unsigned short)'?';
            i++;
            continue;
        }
        i += (size_t)n;
        if (cp < 0x10000) {
            w[o++] = (unsigned short)cp;
        } else {
            unsigned int v = cp - 0x10000;
            w[o++] = (unsigned short)(0xD800 + (v >> 10));
            w[o++] = (unsigned short)(0xDC00 + (v & 0x3FF));
        }
    }
    w[o] = 0;
    if (out_units != NULL) {
        *out_units = o;
    }
    return w;
}

char *a98_utf16_to_utf8(const unsigned short *w, size_t units)
{
    Str out;
    size_t i;
    char tmp[4];
    int n;

    str_init(&out);
    if (w == NULL) {
        return a98_strdup("");
    }
    for (i = 0; i < units; i++) {
        unsigned int cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
        }
        n = a98_utf8_encode(cp, tmp);
        if (n > 0 && str_append(&out, tmp, (size_t)n) != 0) {
            str_free(&out);
            return NULL;
        }
    }
    return str_detach(&out);
}

char *a98_utf8_sanitize(const char *s)
{
    Str out;
    size_t i;
    size_t len;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    if (a98_utf8_valid(s, len)) {
        return NULL;
    }

    str_init(&out);
    i = 0;
    while (i < len) {
        int n = a98_utf8_decode(s + i, len - i, NULL);

        if (n > 0) {
            if (str_append(&out, s + i, (size_t)n) != 0) {
                str_free(&out);
                return NULL;
            }
            i += (size_t)n;
            continue;
        }
        /* Not UTF-8 here: Win32 handed us local-code-page text (a path from
         * FindFirstFileA, a directory name typed in a 9x dialog).  Ask the
         * code page how many bytes one of its characters takes rather than
         * grouping failing bytes - a GBK pair's second byte can itself begin
         * a valid UTF-8 sequence, so a byte-run scan splits the character
         * apart. */
        {
            int nbytes = 1;
            char *chunk;
            char *utf8;

#ifdef _WIN32
            /* One character of the local code page: a DBCS lead byte takes
             * its trail byte with it, everything else is a single byte.
             * Bytes are grouped this way rather than by "what fails to decode
             * as UTF-8", because a GBK pair's second byte can start a valid
             * UTF-8 sequence (D0 C2 .. would split into '?' + U+00BD). */
            if (i + 1 < len && IsDBCSLeadByteEx(CP_ACP, (BYTE)s[i])) {
                nbytes = 2;
            }
#endif
            chunk = a98_strndup(s + i, (size_t)nbytes);
            utf8 = chunk != NULL ? a98_acp_to_utf8(chunk) : NULL;
            if (utf8 != NULL && utf8[0] != '\0') {
                if (str_append_cstr(&out, utf8) != 0) {
                    a98_free(utf8);
                    a98_free(chunk);
                    str_free(&out);
                    return NULL;
                }
            } else {
                /* Not even CP_ACP text: one placeholder keeps the rest of the
                 * line aligned. */
                if (str_append_char(&out, '?') != 0) {
                    a98_free(utf8);
                    a98_free(chunk);
                    str_free(&out);
                    return NULL;
                }
            }
            a98_free(utf8);
            a98_free(chunk);
            i += (size_t)nbytes;
        }
    }
    return str_detach(&out);
}

#ifdef _WIN32
char *a98_utf8_to_acp(const char *s)
{
    wchar_t *w;
    size_t units = 0;
    int need;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    /* sizeof(wchar_t) == 2 is asserted at file scope, so reinterpreting the
     * codec's 16-bit units as wchar_t is safe here (and only here). */
    w = (wchar_t *)a98_utf8_to_utf16(s, &units);
    if (w == NULL) {
        return NULL;
    }
    if (units > (size_t)INT_MAX) {
        a98_free(w);
        return NULL;
    }
    need = WideCharToMultiByte(CP_ACP, 0, w, (int)units,
                               NULL, 0, NULL, NULL);
    if (need <= 0) {
        a98_free(w);
        return a98_strdup("");
    }
    out = (char *)a98_alloc((size_t)need + 1);
    if (out == NULL) {
        a98_free(w);
        return NULL;
    }
    WideCharToMultiByte(CP_ACP, 0, w, (int)units, out, need, NULL, NULL);
    out[need] = '\0';
    a98_free(w);
    return out;
}

char *a98_acp_to_utf8(const char *s)
{
    int need;
    wchar_t *w;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    need = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (need <= 0) {
        return a98_strdup("");
    }
    w = (wchar_t *)a98_alloc(((size_t)need + 1) * sizeof(wchar_t));
    if (w == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, need);
    out = a98_utf16_to_utf8((const unsigned short *)w, (size_t)(need - 1));
    a98_free(w);
    return out;
}
#endif
