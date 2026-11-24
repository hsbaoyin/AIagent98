#include <stdio.h>
#include "counter.h"

static int failures = 0;

static void check(const char *label, int start, int delta, int want_rc,
                  int want_value)
{
    int c = start;
    int rc = counter_add(&c, delta);
    if (rc != want_rc || c != want_value) {
        printf("FAIL: %-20s rc=%d want=%d, value=%d want=%d\n",
               label, rc, want_rc, c, want_value);
        failures++;
        return;
    }
    printf("PASS: %s\n", label);
}

int main(void)
{
    check("positive", 0, 5, 0, 5);
    check("negative", 15, -6, 0, 9);
    check("upper edge", 9, 91, 0, 100);
    check("above upper", 100, 1, -1, 100);
    check("below lower", 0, -1, -1, 0);

    if (counter_add(NULL, 1) != -1) {
        printf("FAIL: null pointer\n");
        failures++;
    } else {
        printf("PASS: null pointer\n");
    }

    if (failures != 0) {
        printf("%d visible test(s) failed\n", failures);
        return 1;
    }
    printf("all visible tests passed\n");
    return 0;
}
