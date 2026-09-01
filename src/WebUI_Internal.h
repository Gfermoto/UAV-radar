/**
 * @file    WebUI_Internal.h
 * @brief   Общие хелперы и константы WebUI (не публичный API).
 *
 * ## Разбиение по translation units
 *
 * | Файл              | Ответственность                                      |
 * |-------------------|------------------------------------------------------|
 * | `WebUI.cpp`       | Lifecycle: begin/stop, webTask, WS broadcast, OTA state machine |
 * | `WebUI_Auth.cpp`  | Auth: Basic, CSRF, rate-limit, lockout, WS ticket issue/consume |
 * | `WebUI_Routes.cpp`| HTTP-маршруты REST/SPA/OTA → `openapi-webui.yaml`    |
 * | `WebUI_Html.cpp`  | Сборка SPA, i18n replace, gzip-кэш в PSRAM           |
 * | `WebUI_Alloc.cpp` | `webUiAlloc` / `webUiFree` / `webUiReplaceAll`       |
 * | `WebUI_page.h`    | Исходный HTML/JS шаблон (`WEBUI_HTML_PAGE`)            |
 *
 * Публичный фасад — `WebUI.h`. Маршруты и auth вызываются только из класса WebUI.
 *
 * @see WebUI.h, openapi-webui.yaml, docs/API_REFERENCE.md
 */
#ifndef WEBUI_INTERNAL_H
#define WEBUI_INTERNAL_H

#include <stddef.h>

/** PSRAM-приоритетный alloc для больших буферов SPA/телеметрии. */
void *webUiAlloc(size_t n);
void webUiFree(void *p);

/** Замена всех вхождений подстроки в growable буфере (i18n placeholders). */
bool webUiReplaceAll(char **buf, size_t *len, size_t *cap,
                     const char *from, const char *to);

/** Интервал push телеметрии по WebSocket (мс). */
#define WS_UPDATE_INTERVAL_MS  1000
/** Период обновления CSRF-токена в SPA (мс). */
#define CSRF_REFRESH_MS        3600000
/** Минимум свободной кучи для приёма новых SPA/WS/TCP соединений. */
#define WEBUI_GUARD_MIN_HEAP   20000

#endif
