#include <stdio.h>
#include <limits.h>
#include "counter.h"

static int pass = 0, total = 0;

static void check(const char *label, int start, int delta, int want_rc,
                  int want_value)
{
    int c = start;
    int rc = counter_add(&c, delta);
    total++;
    if (rc == want_rc && c == want_value) {
        pass++;
        printf("PASS: %s\n", label);
    } else {
        printf("FAIL: %s (rc=%d want=%d, value=%d want=%d)\n",
               label, rc, want_rc, c, want_value);
    }
}

int main(void)
{
    total++;
    if (counter_add(NULL, 1) == -1) {
        pass++;
        printf("PASS: null\n");
    } else {
        printf("FAIL: null\n");
    }
    check("zero delta", 42, 0, 0, 42);
    check("from zero up", 0, 100, 0, 100);
    check("to zero down", 100, -100, 0, 0);
    check("far below", 50, -1000, -1, 50);
    check("far above", 50, 1000, -1, 50);
    check("overflow positive", 50, INT_MAX, -1, 50);
    check("overflow negative", 50, INT_MIN, -1, 50);
    check("keep on failure", 99, 100, -1, 99);
    check("chain ok", 10, 20, 0, 30);
    printf("HIDDEN SCORE: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
