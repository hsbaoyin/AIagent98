#include "jsonutil.h"
#include "../core/str.h"

#include <errno.h>
#include <limits.h>

cJSON *json_parse(const char *text, size_t len)
{
    if (text == NULL) {
        return NULL;
    }
    return cJSON_ParseWithLength(text, len);
}

const char *json_string(cJSON *obj, const char *name)
{
    cJSON *item;
    if (obj == NULL) {
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return NULL;
}

int json_int(cJSON *obj, const char *name, int def)
{
    cJSON *item;
    double v;

    if (obj == NULL) {
        return def;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        v = item->valuedouble;
        /* Converting a double outside the int range (or a NaN/Inf) to int is
         * undefined behaviour.  The negated inclusive check also rejects NaN,
         * for which every comparison is false. */
        if (!(v >= (double)INT_MIN && v <= (double)INT_MAX)) {
            return def;
        }
        return (int)v;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end;
        long n;

        errno = 0;
        n = strtol(item->valuestring, &end, 10);
        /* Require a digit, no trailing junk, no range error, and a value
         * that actually fits in int (long may be wider than int). */
        if (end == item->valuestring || *end != '\0' || errno == ERANGE) {
            return def;
        }
#if LONG_MAX > INT_MAX
        /* On 32-bit Win32 long and int are the same width, so this extra
         * check is both unnecessary and a "comparison always false" warning. */
        if (n < (long)INT_MIN || n > (long)INT_MAX) {
            return def;
        }
#endif
        return (int)n;
    }
    return def;
}

int json_bool(cJSON *obj, const char *name, int def)
{
    cJSON *item;
    if (obj == NULL) {
        return def;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0.0;
    }
    return def;
}
