/**
 * @file    TlsUtil.h
 * @brief   Общая настройка WiFiClientSecure: CA из NVS (PEM).
 *
 * configure() загружает CA в буфер клиента; без CA — false, TLS запрещён.
 * invalidate() сбрасывает кэш CA (переподключение после смены cert в NVS).
 *
 * @see WebCredentials.h, docs/ARCHITECTURE.md
 */

#ifndef TLS_UTIL_H
#define TLS_UTIL_H

#include <WiFiClientSecure.h>

namespace TlsUtil {

/**
 * @brief Применить TLS к клиенту (CA копируется во внутренний буфер).
 * @return true если загружен CA из NVS, false если отказать в TLS.
 */
bool configure(WiFiClientSecure &client);

/** Drop cached CA; callers must invalidate WiFiClientSecure / reconnect. */
void invalidate();

}  // namespace TlsUtil

#endif
