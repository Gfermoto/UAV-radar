/**
 * @file    WebUI_Auth.cpp
 * @brief   WebUI: Basic Auth, CSRF, rate-limit, lockout, WS ticket.
 *
 * Уровни: `requireAuth` → `requireHardenedAuth` → `requireCsrf` /
 * `requireMutable`. Билеты — `WsTicketAuth`. Учётные данные — `WebCredentials`.
 * См. `openapi-webui.yaml` (securitySchemes), `docs/API_REFERENCE.md` §2.
 */
#include "WebUI.h"
#include "WebUI_Internal.h"
#include "WebCredentials.h"
#include "ValidateUtil.h"
#include "WsTicketAuth.h"
#include <cstring>

bool WebUI::isAuthLocked(uint32_t clientIp) const {
    const uint32_t now = millis();
    for (size_t i = 0; i < AUTH_LOCK_SLOTS; ++i) {
        if (_authLocks[i].ip == clientIp) {
            return _authLocks[i].lockUntilMs != 0 &&
                   (int32_t)(now - _authLocks[i].lockUntilMs) < 0;
        }
    }
    return false;
}

void WebUI::noteAuthFailure(uint32_t clientIp) const {
    const uint32_t now = millis();
    int freeIdx = -1;
    size_t oldestIdx = 0;
    bool haveOldest = false;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < AUTH_LOCK_SLOTS; ++i) {
        if (_authLocks[i].ip == clientIp) {
            if (_authLocks[i].fails < 255) _authLocks[i].fails++;
            _authLocks[i].lastTouchMs = now;
            if (_authLocks[i].fails >= AUTH_FAIL_LIMIT) {
                _authLocks[i].lockUntilMs = now + AUTH_LOCKOUT_MS;
            }
            return;
        }
        const bool locked = _authLocks[i].lockUntilMs != 0 &&
            (int32_t)(now - _authLocks[i].lockUntilMs) < 0;
        if (locked) continue;  // never evict an active lockout
        if (freeIdx < 0 && _authLocks[i].ip == 0) freeIdx = (int)i;
        const uint32_t age = now - _authLocks[i].lastTouchMs;
        if (!haveOldest || age >= oldestAge) {
            oldestAge = age;
            oldestIdx = i;
            haveOldest = true;
        }
    }
    // Все слоты в active lockout — не вытесняем locked IP.
    if (freeIdx < 0 && !haveOldest) return;
    const size_t idx = (freeIdx >= 0) ? (size_t)freeIdx : oldestIdx;
    _authLocks[idx].ip = clientIp;
    _authLocks[idx].fails = 1;
    _authLocks[idx].lockUntilMs = 0;
    _authLocks[idx].lastTouchMs = now;
}

void WebUI::noteAuthSuccess(uint32_t clientIp) const {
    for (size_t i = 0; i < AUTH_LOCK_SLOTS; ++i) {
        if (_authLocks[i].ip == clientIp) {
            _authLocks[i].fails = 0;
            _authLocks[i].lockUntilMs = 0;
            _authLocks[i].lastTouchMs = millis();
            return;
        }
    }
}

bool WebUI::requireAuth(AsyncWebServerRequest *request) const {
    if (!request || !request->client()) return false;
    const uint32_t ip = (uint32_t)request->client()->remoteIP();
    if (ip && isAuthLocked(ip)) {
        request->send(429, "application/json",
                      "{\"status\":\"auth_locked\",\"retry_ms\":60000}");
        return false;
    }
    char user[WEB_CRED_USER_MAX + 1];
    char pass[WEB_CRED_PASS_MAX];
    WebCredentials::load(user, sizeof(user), pass, sizeof(pass));
    // Constant-time verify (request->authenticate сравнивает не CT — timing side-channel)
    String authHdr = request->hasHeader("Authorization")
        ? request->getHeader("Authorization")->value() : "";
    if (authHdr.length() && WebCredentials::verifyBasicAuth(authHdr.c_str(), user, pass)) {
        if (ip) noteAuthSuccess(ip);
        return true;
    }
    // Считать failure только при реальной попытке (есть Authorization).
    // Иначе SPA/prefetch без credentials → self-lockout.
    if (ip && request->hasHeader("Authorization")) {
        noteAuthFailure(ip);
    }
    request->requestAuthentication("RTSPMIC");
    return false;
}

bool WebUI::requireHardenedAuth(AsyncWebServerRequest *request) const {
    if (!requireAuth(request)) return false;
    if (WebCredentials::isDefaultPassword()) {
        request->send(403, "application/json",
                      "{\"status\":\"must_change_password\"}");
        return false;
    }
    return true;
}

bool WebUI::requireCsrf(AsyncWebServerRequest *request) const {
    char local[CSRF_TOKEN_LEN];
    local[0] = '\0';
    if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(local, _csrfToken, sizeof(local) - 1);
        local[sizeof(local) - 1] = '\0';
        xSemaphoreGive(_wsMutex);
    } else {
        strncpy(local, _csrfToken, sizeof(local) - 1);
        local[sizeof(local) - 1] = '\0';
    }
    if (request->hasHeader("X-CSRF-Token")) {
        const String &token = request->getHeader("X-CSRF-Token")->value();
        if (WebCredentials::ctEq(token.c_str(), local) && local[0] != '\0') return true;
    }
    request->send(403, "application/json", "{\"status\":\"bad_csrf\"}");
    return false;
}

bool WebUI::requireMutable(AsyncWebServerRequest *request) const {
    if (!requireAuth(request)) return false;
    if (WebCredentials::isDefaultPassword()) {
        request->send(403, "application/json",
                      "{\"status\":\"must_change_password\"}");
        return false;
    }
    return requireCsrf(request);
}

bool WebUI::validateUrl(const char *url, size_t maxLen) const {
    return ValidateUtil::validateUrl(url, maxLen);
}

bool WebUI::validateHostname(const char *host, size_t maxLen) const {
    return ValidateUtil::validateHostname(host, maxLen);
}

bool WebUI::requireWsAuth(AsyncWebSocketClient *client) {
    if (!client) return false;
    if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const bool ok = isWsClientAuthed(client->id());
    if (_wsMutex) xSemaphoreGive(_wsMutex);
    return ok;
}

void WebUI::issueWsTicket(uint32_t clientIp, char *out, size_t outLen) {
    if (!out || outLen < WS_TICKET_LEN) return;
    if (!WsTicketAuth::issue(_wsTicketKey, sizeof(_wsTicketKey), clientIp,
                             millis(), WS_TICKET_TTL_MS, esp_random(),
                             out, outLen)) {
        out[0] = '\0';
    }
}

bool WebUI::consumeWsTicket(uint32_t clientIp, const char *ticket) {
    // Nonce store is shared — serialize consume against concurrent WS auth.
    if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const bool ok = WsTicketAuth::verify(_wsTicketKey, sizeof(_wsTicketKey),
                                         clientIp, millis(), ticket, &_wsTicketNonces);
    if (_wsMutex) xSemaphoreGive(_wsMutex);
    return ok;
}

bool WebUI::isWsClientAuthed(uint32_t clientId) const {
    for (uint8_t i = 0; i < _wsAuthedCount; ++i) {
        if (_wsAuthedIds[i] == clientId) return true;
    }
    return false;
}

bool WebUI::authorizeWsClient(AsyncWebSocketClient *client, const char *ticket) {
    if (!client || !ticket) return false;
    // Ticket verify + authed-id mutate under one lock (avoid race with broadcast).
    if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const bool okTicket = WsTicketAuth::verify(
        _wsTicketKey, sizeof(_wsTicketKey),
        (uint32_t)client->remoteIP(), millis(), ticket, &_wsTicketNonces);
    if (!okTicket) {
        if (_wsMutex) xSemaphoreGive(_wsMutex);
        return false;
    }
    const uint32_t id = client->id();
    if (!isWsClientAuthed(id)) {
        if (_wsAuthedCount >= WS_AUTHED_SLOTS) {
            memmove(_wsAuthedIds, _wsAuthedIds + 1,
                    (_wsAuthedCount - 1) * sizeof(_wsAuthedIds[0]));
            _wsAuthedCount--;
        }
        _wsAuthedIds[_wsAuthedCount++] = id;
    }
    if (_wsMutex) xSemaphoreGive(_wsMutex);
    return true;
}

bool WebUI::checkRateLimit(uint32_t clientIp, uint32_t bucket, uint32_t minIntervalMs) {
    const uint32_t now = millis();
    int freeIdx = -1;
    size_t oldestIdx = 0;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < RATE_SLOTS; ++i) {
        if (_rateSlots[i].ip == clientIp && _rateSlots[i].bucket == bucket) {
            if ((now - _rateSlots[i].lastMs) < minIntervalMs) return false;
            _rateSlots[i].lastMs = now;
            return true;
        }
        if (freeIdx < 0 && _rateSlots[i].ip == 0) freeIdx = (int)i;
        // Wrap-safe age: prefer reclaiming the least-recently-seen slot.
        const uint32_t age = now - _rateSlots[i].lastMs;
        if (age >= oldestAge) {
            oldestAge = age;
            oldestIdx = i;
        }
    }
    const size_t idx = (freeIdx >= 0) ? (size_t)freeIdx : oldestIdx;
    _rateSlots[idx].ip = clientIp;
    _rateSlots[idx].bucket = bucket;
    _rateSlots[idx].lastMs = now;
    return true;
}

bool WebUI::requireRateOk(AsyncWebServerRequest *request, uint32_t minIntervalMs) {
    if (!request || !request->client()) return false;
    const uint32_t ip = (uint32_t)request->client()->remoteIP();
    // Bucket = FNV-1a по URL — csrf/mel/ticket не делят один слот.
    uint32_t bucket = 2166136261u;
    const String &url = request->url();
    for (size_t i = 0; i < url.length(); ++i) {
        bucket ^= (uint8_t)url[i];
        bucket *= 16777619u;
    }
    if (!checkRateLimit(ip, bucket, minIntervalMs)) {
        request->send(429, "application/json", "{\"status\":\"rate_limited\"}");
        return false;
    }
    return true;
}

void WebUI::refreshCsrf() {
    char next[CSRF_TOKEN_LEN];
    for (int i = 0; i < CSRF_TOKEN_LEN - 1; i++) {
        int r = esp_random() % 36;
        next[i] = (r < 10) ? ('0' + r) : ('a' + r - 10);
    }
    next[CSRF_TOKEN_LEN - 1] = '\0';
    if (!_wsMutex || xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        memcpy(_csrfToken, next, CSRF_TOKEN_LEN);
        return;
    }
    memcpy(_csrfToken, next, CSRF_TOKEN_LEN);
    xSemaphoreGive(_wsMutex);
}

void WebUI::scheduleRestart(uint32_t delayMs) {
    uint32_t at = millis() + delayMs;
    if (at == 0) at = 1;  // 0 — sentinel «нет restart»; wrap в 0 блокировал reboot
    _restartAtMs.store(at);
}
