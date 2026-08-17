/**
 * @file    WebCredentials.h
 * @brief   Общие учётные данные WebUI Basic Auth и RTSP Basic (NVS).
 *
 * ## Назначение
 *
 * Единый источник логина/пароля для:
 *   - WebUI HTTP Basic (`WebUI::requireAuth`)
 *   - RTSP-сервер (`RTSPServer::requireRtspAuth`)
 *
 * Ключи в NVS namespace **`rtspmic`**: `web_user`, `web_pass`.
 * Дефолты — `WEB_UI_USER` / `WEB_UI_PASSWORD` из `Config.h`.
 *
 * ## Контракт
 *
 *   - `validateUser` / `validatePass` — правила перед `save`
 *   - `isDefaultPassword` — блокирует «чувствительные» эндпоинты и RTSP до смены пароля
 *   - `verifyBasicAuth` / `parseBasicAuthHeader` — разбор `Authorization: Basic`
 *   - `ctEq` — сравнение без утечки по времени
 *
 * @see WebUI_Auth.cpp, RTSPServer, docs/API_REFERENCE.md §5
 */

#ifndef WEB_CREDENTIALS_H
#define WEB_CREDENTIALS_H

#include <stddef.h>
#include <stdint.h>

#ifndef WEB_CRED_USER_MAX
#define WEB_CRED_USER_MAX 32
#endif
#ifndef WEB_CRED_PASS_MAX
#define WEB_CRED_PASS_MAX 64
#endif

namespace WebCredentials {

/** @brief Валидация имени: 1..32 символа [A-Za-z0-9_.-]. */
bool validateUser(const char *user);

/** @brief Валидация пароля: длина 8..WEB_CRED_PASS_MAX-1, без ':' (Basic user:pass). */
bool validatePass(const char *pass);

/** @brief Загрузка из NVS `rtspmic` (дефолты WEB_UI_USER / WEB_UI_PASSWORD). */
bool load(char *user, size_t userLen, char *pass, size_t passLen);

/** @brief Сохранение в NVS после validateUser + validatePass. */
bool save(const char *user, const char *pass);

/** @return true если загруженный пароль совпадает с WEB_UI_DEFAULT_PASSWORD. */
bool isDefaultPassword();

/** @brief Сравнение строк в постоянном времени (NUL-terminated). */
bool ctEq(const char *a, const char *b);

/**
 * @brief Разбор "Authorization: Basic <b64>" или "Basic <b64>".
 * @param authHeader  Строка заголовка или только значение Basic.
 * @param outUser     Буфер для user (NUL-terminated).
 * @param outPass     Буфер для pass (NUL-terminated).
 * @return true если декодированы непустые user и pass.
 */
bool parseBasicAuthHeader(const char *authHeader,
                          char *outUser, size_t userLen,
                          char *outPass, size_t passLen);

/**
 * @brief Проверка заголовка Authorization против ожидаемых user/pass.
 * @return true при совпадении (constant-time для обоих полей).
 */
bool verifyBasicAuth(const char *authHeader, const char *user, const char *pass);

}  // namespace WebCredentials

#endif
