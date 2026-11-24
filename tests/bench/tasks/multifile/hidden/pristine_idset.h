#ifndef IDSET_H
#define IDSET_H

#include <stddef.h>

/* Append a nonnegative id to ids[0..*count), capacity slots, no duplicates.
 * Return 0 on success (incrementing *count), -1 on any invalid request. */
int idset_add(int *ids, int *count, int capacity, int id);

/* Format ids as "[1,2,3]" (or "[]" when empty) into out, NUL-terminated.
 * Return the character count, or -1 if out is NULL, out_size is 0, args are
 * invalid, or the buffer is too small. */
int idfmt_write(const int *ids, int count, char *out, size_t out_size);

#endif
