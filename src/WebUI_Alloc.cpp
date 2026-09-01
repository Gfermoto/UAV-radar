/**
 * @file    WebUI_Alloc.cpp
 * @brief   WebUI: PSRAM/DRAM alloc и replace-хелперы для SPA/i18n.
 */
#include "WebUI_Internal.h"
#include <cstring>
#include <esp_heap_caps.h>

void *webUiAlloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

void webUiFree(void *p) {
    if (p) heap_caps_free(p);
}

bool webUiReplaceAll(char **buf, size_t *len, size_t *cap,
                     const char *from, const char *to) {
    if (!buf || !*buf || !len || !cap || !from || !to) return false;
    const size_t fl = strlen(from);
    const size_t tl = strlen(to);
    if (fl == 0) return true;

    if (fl == tl) {
        for (char *p = *buf; (p = strstr(p, from)) != nullptr; p += tl) {
            memcpy(p, to, tl);
        }
        return true;
    }

    size_t count = 0;
    for (char *p = *buf; (p = strstr(p, from)) != nullptr; p += fl) {
        ++count;
    }
    if (count == 0) return true;

    const size_t newLen = *len + count * tl - count * fl;
    char *out = static_cast<char *>(webUiAlloc(newLen + 1));
    if (!out) return false;

    char *dst = out;
    const char *src = *buf;
    while (const char *hit = strstr(src, from)) {
        const size_t chunk = static_cast<size_t>(hit - src);
        memcpy(dst, src, chunk);
        dst += chunk;
        memcpy(dst, to, tl);
        dst += tl;
        src = hit + fl;
    }
    const size_t rem = strlen(src);
    memcpy(dst, src, rem + 1);

    webUiFree(*buf);
    *buf = out;
    *cap = newLen + 1;
    *len = newLen;
    return true;
}
