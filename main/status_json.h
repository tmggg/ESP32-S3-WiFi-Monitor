#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define STATUS_JSON_SAFE_INTEGER 9007199254740991ULL

/* cJSON stores numbers as double. Quote large unsigned integer tokens BEFORE
 * parsing, so counters/sequence numbers never pass through floating point.
 * Strings are skipped verbatim and the resulting document is still validated
 * by cJSON. The server's ordinary decimal integer representation is preserved. */
static inline const char *status_json_large_integer_end(const char *begin, const char *p)
{
    if (*p < '1' || *p > '9' ||
        (p != begin && !strchr("[: ,\t\r\n", p[-1]))) return NULL;
    const char *end = p;
    while (*end >= '0' && *end <= '9') ++end;
    if (end - p < 16 || (*end && !strchr(",]} \t\r\n", *end))) return NULL;
    return end;
}

static inline cJSON *status_json_parse(const char *input)
{
    size_t extra = 0;
    for (const char *p = input; *p;) {
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) ++p;
                ++p;
            }
            if (*p) ++p;
            continue;
        }
        const char *end = status_json_large_integer_end(input, p);
        if (end) { extra += 2; p = end; }
        else ++p;
    }
    if (!extra) return cJSON_ParseWithOpts(input, NULL, true);
    char *exact = malloc(strlen(input) + extra + 1);
    if (!exact) return NULL;
    char *out = exact;
    for (const char *p = input; *p;) {
        if (*p == '"') {
            *out++ = *p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) *out++ = *p++;
                *out++ = *p++;
            }
            if (*p) *out++ = *p++;
            continue;
        }
        const char *end = status_json_large_integer_end(input, p);
        if (end) {
            *out++ = '"';
            while (p != end) *out++ = *p++;
            *out++ = '"';
        } else *out++ = *p++;
    }
    *out = 0;
    cJSON *root = cJSON_ParseWithOpts(exact, NULL, true);
    free(exact);
    return root;
}

static inline bool status_json_u64(cJSON *root, const char *name, uint64_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring) {
        const unsigned char *p = (const unsigned char *)item->valuestring;
        if (!*p || (p[0] == '0' && p[1])) return false;
        uint64_t result = 0;
        for (; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            unsigned digit = *p - '0';
            if (result > (UINT64_MAX - digit) / 10) return false;
            result = result * 10 + digit;
        }
        *value = result;
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > (double)STATUS_JSON_SAFE_INTEGER ||
        floor(item->valuedouble) != item->valuedouble) return false;
    *value = (uint64_t)item->valuedouble;
    return true;
}
