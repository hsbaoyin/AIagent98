#include <stdio.h>
#include "strutil.h"

static int pass = 0, total = 0;

static void check(const char *label, const char *s, int max, int want_count,
                  const int *want_vals)
{
    int out[32];
    int n, i, ok;
    total++;
    for (i = 0; i < 32; i++) out[i] = 12345;
    n = csv_parse_int_list(s, out, max);
    ok = (n == want_count);
    if (ok && want_count > 0) {
        for (i = 0; i < want_count; i++) {
            if (out[i] != want_vals[i]) { ok = 0; break; }
        }
    }
    if (ok) { pass++; printf("PASS: %s\n", label); }
    else printf("FAIL: %s (n=%d want=%d, out[0]=%d)\n", label, n, want_count, out[0]);
}

int main(void)
{
    { const int v[] = {1,2,3,4,5}; check("whitespace-only max", " 1 , 2 , 3 , 4 , 5 ", 5, 5, v); }
    { const int v[] = {7};         check("leading zeros", "007", 4, 1, v); }
    { const int v[] = {-42};       check("negative spaced", "  -42  ", 4, 1, v); }
    { const int v[] = {10,-20,30}; check("mixed", "10, -20 ,+30", 4, 3, v); }
    check("empty string", "", 4, 0, NULL);
    check("spaces only", "   \t  ", 4, 0, NULL);
    check("leading comma", ",1", 4, -1, NULL);
    check("trailing comma", "1 ,", 4, -1, NULL);
    check("double comma", "1,,2", 4, -1, NULL);
    check("letter token", "1,2x", 4, -1, NULL);
    check("float token", "1,2.5", 4, -1, NULL);
    check("overflow token", "999999999999999999999", 4, -1, NULL);
    check("too many for max", "1,2,3,4,5", 4, -1, NULL);
    check("null input", NULL, 4, -1, NULL);
    check("zero max nonempty", "1", 0, -1, NULL);
    printf("HIDDEN SCORE: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
