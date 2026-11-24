#ifndef STRUTIL_H
#define STRUTIL_H

#include <stddef.h>

/* Parse a comma-separated list of integers into out[] (at most max items).
 * Leading/trailing whitespace around the whole string and around each number
 * is allowed. Valid tokens are optional sign followed by decimal digits.
 * Return the number of items parsed, or -1 if any token is invalid, empty, or
 * the list contains more than max items. Empty / whitespace-only input
 * returns 0. */
int csv_parse_int_list(const char *text, int *out, int max);

#endif
