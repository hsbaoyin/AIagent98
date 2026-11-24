#include <stdio.h>
#include <string.h>
#include "strutil.h"

static int failures = 0;

static void check(const char *label, const char *s, int want_count,
                  const int *want_vals)
{
    int out[16];
    int n = csv_parse_int_list(s, out, 16);
    int i;
    if (n != want_count) {
        printf("FAIL: %-22s count=%d want=%d\n", label, n, want_count);
        failures++;
        return;
    }
    for (i = 0; i < n && i < 16; i++) {
        if (out[i] != want_vals[i]) {
            printf("FAIL: %-22s out[%d]=%d want=%d\n", label, i, out[i], want_vals[i]);
            failures++;
            return;
        }
    }
    printf("PASS: %s\n", label);
}

int main(void)
{
    { const int v[] = {1, 2, 3};   check("simple",       "1,2,3",   3, v); }
    { const int v[] = {4, 5};      check("spaces",       " 4 , 5 ", 2, v); }
    { const int v[] = {-7, 8};     check("negatives",    "-7,+8",   2, v); }
    { const int v[] = {0};         check("single zero",  "0",       1, v); }
    check("empty", "", 0, NULL);
    check("invalid token", "abc", -1, NULL);
    check("empty token", "1,,2", -1, NULL);
    check("trailing comma", "1,", -1, NULL);

    if (failures != 0) {
        printf("%d visible test(s) failed\n", failures);
        return 1;
    }
    printf("all visible tests passed\n");
    return 0;
}
