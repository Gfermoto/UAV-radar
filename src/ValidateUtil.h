/**
 * @file    ValidateUtil.h
 * @brief   Валидация URL и hostname для WebUI/MQTT (без shell-инъекций).
 *
 * validateUrl — только https://, printable, без кавычек/бэкслеша.
 * validateHostname — alnum, ., -, :.
 *
 * @see WebUI.h, docs/ARCHITECTURE.md
 */

#ifndef VALIDATE_UTIL_H
#define VALIDATE_UTIL_H

#include <cstddef>
#include <cstring>
#include <cctype>

namespace ValidateUtil {

/** HTTPS-only URL. Rejects http://, empty, non-printable, shell metas. */
inline bool validateUrl(const char *url, size_t maxLen) {
    if (!url || !url[0]) return false;
    const size_t len = strnlen(url, maxLen);
    if (len == 0 || len >= maxLen) return false;
    if (strncmp(url, "https://", 8) != 0) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)url[i];
        if (!std::isprint(c) || c == '\'' || c == '"' || c == '`' || c == '\\') return false;
    }
    return true;
}

/** Hostname: alphanum, ., -, : only. */
inline bool validateHostname(const char *host, size_t maxLen) {
    if (!host || !host[0]) return false;
    const size_t len = strnlen(host, maxLen);
    if (len == 0 || len >= maxLen) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)host[i];
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':')) return false;
    }
    return true;
}

}  // namespace ValidateUtil

#endif
