#include "counter.h"

int counter_add(int *counter, int delta)
{
    if (counter == NULL)
        return -1;
    /* BUG: only the upper bound is checked, and int addition may overflow. */
    if (*counter + delta > 100)
        return -1;
    *counter += delta;
    return 0;
}
