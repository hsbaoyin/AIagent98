#include <stdio.h>
#include <string.h>
#include "idset.h"

static int pass = 0, total = 0;

static void addcheck(const char *label, int start, int cap, int id,
                     int want_rc, int want_count)
{
    int ids[8];
    int count = start;
    int i;
    for (i = 0; i < 8; i++) ids[i] = -1;
    ids[0] = 7; ids[1] = 11; ids[2] = 13;
    total++;
    if (idset_add(ids, &count, cap, id) == want_rc && count == want_count) {
        pass++;
        printf("PASS: %s\n", label);
    } else {
        printf("FAIL: %s (count=%d want=%d)\n", label, count, want_count);
    }
}

static void fmtcheck(const char *label, const int *ids, int count,
                     size_t size, const char *want, int want_rc)
{
    char out[32];
    int rc;
    out[0] = 'X';
    total++;
    rc = idfmt_write(ids, count, out, size);
    if (rc == want_rc && (rc < 0 || (size > 0 && strcmp(out, want) == 0))) {
        pass++;
        printf("PASS: %s\n", label);
    } else {
        printf("FAIL: %s (rc=%d want=%d, out='%s')\n", label, rc, want_rc, out);
    }
}

int main(void)
{
    const int ids3[] = {7, 11, 13};
    const int with_negative[] = {7, -1};
    int ids[8] = {0};
    int count = 0;

    total++;
    if (idset_add(NULL, &count, 8, 1) == -1 && count == 0) {
        pass++; printf("PASS: null ids\n");
    } else printf("FAIL: null ids\n");

    total++;
    count = 0;
    if (idset_add(ids, NULL, 8, 1) == -1) {
        pass++; printf("PASS: null count\n");
    } else printf("FAIL: null count\n");

    addcheck("zero capacity", 0, 0, 1, -1, 0);
    addcheck("bad count", 9, 8, 1, -1, 9);
    addcheck("negative id", 0, 8, -5, -1, 0);
    addcheck("duplicate first", 3, 8, 7, -1, 3);
    addcheck("duplicate middle", 3, 8, 11, -1, 3);
    addcheck("exact fill", 3, 4, 99, 0, 4);

    fmtcheck("three ids", ids3, 3, 32, "[7,11,13]", 9);
    fmtcheck("exact buffer", ids3, 3, 10, "[7,11,13]", 9);
    fmtcheck("small buffer", ids3, 3, 9, "", -1);
    fmtcheck("empty exact", NULL, 0, 2, "[]", 2);
    fmtcheck("empty small", NULL, 0, 1, "", -1);
    fmtcheck("null out", ids3, 3, 0, "", -1);
    fmtcheck("negative count", ids3, -1, 32, "", -1);
    fmtcheck("negative id", with_negative, 2, 32, "", -1);
    printf("HIDDEN SCORE: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
