/*
 * AIagent98 - UTF-8 helpers.
 *
 * Win9x has no Unicode Win32 API worth using, so we convert UTF-8 to the
 * local ANSI code page (ACP) for display and back to UTF-8 for input.
 * The codec itself is portable; only the ACP wrappers need Windows.
 */
#ifndef A98_UTF8_H
#define A98_UTF8_H

#include "base.h"

/* Return 1 when the whole buffer is valid UTF-8. */
int a98_utf8_valid(const char *s, size_t len);

/* Decode one code point. Returns bytes consumed (1..4), or -1 on error. */
int a98_utf8_decode(const char *s, size_t len, unsigned int *cp);

/* Encode one code point into out[0..3]. Returns byte count or -1. */
int a98_utf8_encode(unsigned int cp, char out[4]);

/* Convert UTF-8 to UTF-16. Returns a malloc'ed NUL-terminated buffer
 * (16-bit code units), or NULL on failure. */
unsigned short *a98_utf8_to_utf16(const char *s, size_t *out_units);

/* Convert UTF-16 to UTF-8. Returns malloc'ed NUL-terminated string. */
char *a98_utf16_to_utf8(const unsigned short *w, size_t units);

#ifdef _WIN32
/* UTF-8 <-> local ANSI code page, for Win32 text APIs. */
/* Repair text that is not valid UTF-8.  Every run of bytes that fails to
 * decode is re-read as CP_ACP text and written back out as UTF-8; the rest of
 * the string is copied through untouched, so already-UTF-8 content survives.
 * Returns a malloc'ed string, or NULL when the input was already valid (there
 * is nothing to fix). */
char *a98_utf8_sanitize(const char *s);

char *a98_utf8_to_acp(const char *s);
char *a98_acp_to_utf8(const char *s);
#endif

#endif /* A98_UTF8_H */
