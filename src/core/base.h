/*
 * AIagent98 - base definitions and small helpers.
 *
 * Target: Windows 95/98/ME (Win32), 32-bit, built with OpenWatcom.
 * Keep this header free of Windows types so it can be reused by host tools.
 */
#ifndef A98_BASE_H
#define A98_BASE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#define A98_VERSION "0.3.0"
#define A98_NAME "AIagent98"

/* Compile-time build stamp.  __DATE__/__TIME__ are standard predefined
 * macros (supported by OpenWatcom, MinGW and MSVC), so the About dialog and
 * the startup log always show the exact build that is running. */
#define A98_BUILD_DATE  __DATE__
#define A98_BUILD_TIME  __TIME__
#define A98_BUILD_STAMP __DATE__ " " __TIME__

/* Longest path we accept. Win9x MAX_PATH is 260. */
#define A98_MAX_PATH 260

#define A98_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define A98_MIN(a, b) ((a) < (b) ? (a) : (b))
#define A98_MAX(a, b) ((a) > (b) ? (a) : (b))
#define A98_UNUSED(x) ((void)(x))

void *a98_alloc(size_t n);
void *a98_realloc(void *p, size_t n);
void a98_free(void *p);
char *a98_strdup(const char *s);
char *a98_strndup(const char *s, size_t n);

int a98_strcasecmp(const char *a, const char *b);
int a98_strncasecmp(const char *a, const char *b, size_t n);

/* Format into a freshly allocated string, or NULL on failure. */
char *a98_strfmt(const char *fmt, ...);

#endif /* A98_BASE_H */
