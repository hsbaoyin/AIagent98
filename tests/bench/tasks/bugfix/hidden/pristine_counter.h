#ifndef COUNTER_H
#define COUNTER_H

#include <stddef.h>

/* Add delta to *counter, keeping it within [0, 100].
 * Return 0 on success, -1 for NULL or an out-of-range / overflowing update;
 * on failure *counter must be unchanged. */
int counter_add(int *counter, int delta);

#endif
