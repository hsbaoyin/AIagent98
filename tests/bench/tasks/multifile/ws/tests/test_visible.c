#include <stdio.h>
#include <string.h>
#include "idset.h"

static int failures = 0;

static void check_add(const char *label, int start_count, int capacity, int id,
                      int want_rc, int want_count)
{
    int ids[8];
    int count = start_count;
    int i;
    for (i = 0; i < 8; i++) ids[i] = -1;
    ids[0] = 1; ids[1] = 2;
    if (idset_add(ids, &count, capacity, id) != want_rc || count != want_count) {
        printf("FAIL: %-18s rc/count mismatch (count=%d want=%d)\n",
               label, count, want_count);
        failures++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void check_fmt(const char *label, const int *ids, int count,
                      const char *want, int want_rc)
{
    char out[32];
    int rc = idfmt_write(ids, count, out, sizeof(out));
    if ((rc != want_rc) || (rc >= 0 && strcmp(out, want) != 0)) {
        printf("FAIL: %-18s rc=%d want=%d, out='%s'\n", label, rc, want_rc, out);
        failures++;
        return;
    }
    printf("PASS: %s\n", label);
}

int main(void)
{
    check_add("append", 2, 8, 3, 0, 3);
    check_add("duplicate", 2, 8, 2, -1, 2);
    check_add("full", 2, 2, 5, -1, 2);
    check_add("negative id", 0, 8, -1, -1, 0);

    { const int v[] = {1, 2, 3}; check_fmt("format three", v, 3, "[1,2,3]", 7); }
    check_fmt("format empty", NULL, 0, "[]", 2);

    if (failures != 0) {
        printf("%d visible test(s) failed\n", failures);
        return 1;
    }
    printf("all visible tests passed\n");
    return 0;
}
