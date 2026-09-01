/**
 * @file    WsTicketAuth.h
 * @brief   HMAC-билеты для авторизации WebSocket (host-testable).
 *
 * ## Контракт билета
 *
 * Формат: `v | expiry | ip | nonce | mac32` (58 символов)
 *   - `v`      — версия ('2')
 *   - `expiry` — 8 hex, deadline millis()
 *   - `ip`     — 8 hex, IPv4 клиента (привязка к сессии)
 *   - `nonce`  — 8 hex, случайное (single-use через NonceStore)
 *   - `mac32`  — первые 16 байт HMAC-SHA256(key, payload) в hex
 *
 * ## Использование
 *
 *   - `issue()` — выдача в `GET /api/ws-ticket` (после hardened auth)
 *   - `verify()` — проверка при WS upgrade; с `NonceStore` — одноразовый nonce
 *   - `NonceStore::consume` — fail-closed при replay или переполнении слотов
 *
 * Ключ — `_wsTicketKey` в WebUI (32 байта, random at begin).
 * TTL по умолчанию: `kDefaultTtlMs` (10 с).
 *
 * @see WebUI_Auth.cpp, openapi-webui.yaml `/api/ws-ticket`, `/ws`
 */
#ifndef WS_TICKET_AUTH_H
#define WS_TICKET_AUTH_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(UNIT_TEST)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include <mbedtls/md.h>
#endif

namespace WsTicketAuth {

static constexpr char kVersion = '2';
static constexpr size_t kPayloadLen = 25;   // v + 24 hex
static constexpr size_t kMacBytes = 16;
static constexpr size_t kMacHexLen = kMacBytes * 2;  // 32
static constexpr size_t kTicketLen = kPayloadLen + kMacHexLen + 1;  // 58
static constexpr uint32_t kDefaultTtlMs = 10000;
static constexpr size_t kNonceSlots = 32;

/** Хранилище использованных nonce до expiry билета (защита от replay). */
struct NonceStore {
    uint32_t nonce[kNonceSlots]{};
    uint32_t expiry[kNonceSlots]{};
    uint8_t count{0};

    void purgeExpired(uint32_t nowMs) {
        uint8_t w = 0;
        for (uint8_t i = 0; i < count; ++i) {
            if ((int32_t)(nowMs - expiry[i]) < 0) {
                nonce[w] = nonce[i];
                expiry[w] = expiry[i];
                ++w;
            }
        }
        count = w;
    }

    bool seen(uint32_t n) const {
        for (uint8_t i = 0; i < count; ++i) {
            if (nonce[i] == n) return true;
        }
        return false;
    }

    /**
     * @brief Записать nonce до expiry билета.
     * @return false если nonce уже seen ИЛИ store полон (fail-closed:
     *         не вытесняем живой nonce — иначе replay до TTL).
     */
    bool consume(uint32_t n, uint32_t ticketExpiry, uint32_t nowMs) {
        purgeExpired(nowMs);
        if (seen(n)) return false;
        if (count >= kNonceSlots) return false;
        nonce[count] = n;
        expiry[count] = ticketExpiry;
        ++count;
        return true;
    }
};

inline bool constTimeHexEq(const char *a, const char *b, size_t n) {
    if (!a || !b) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    diff |= (uint8_t)a[n] ^ (uint8_t)b[n];
    return diff == 0;
}

inline bool hmacSha256Hex(const uint8_t *key, size_t keyLen,
                          const char *payload, char *macHex, size_t macHexLen) {
    if (!key || !payload || !macHex || macHexLen < kMacHexLen + 1) return false;
    uint8_t mac[32];
#if defined(UNIT_TEST)
    unsigned int macLen = 0;
    if (!HMAC(EVP_sha256(), key, (int)keyLen,
              (const unsigned char *)payload, strlen(payload),
              mac, &macLen) || macLen < kMacBytes) {
        return false;
    }
#else
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_hmac(info, key, keyLen,
                                 (const uint8_t *)payload, strlen(payload),
                                 mac) != 0) {
        return false;
    }
#endif
    for (size_t i = 0; i < kMacBytes; ++i) {
        snprintf(macHex + i * 2, 3, "%02x", mac[i]);
    }
    macHex[kMacHexLen] = '\0';
    return true;
}

inline bool formatPayload(char *payload, size_t payloadLen,
                          uint32_t expiryMs, uint32_t clientIp, uint32_t nonce) {
    if (!payload || payloadLen < kPayloadLen + 1) return false;
    int n = snprintf(payload, payloadLen, "%c%08lx%08lx%08lx",
                     kVersion,
                     (unsigned long)expiryMs,
                     (unsigned long)clientIp,
                     (unsigned long)nonce);
    return n == (int)kPayloadLen;
}

/** @brief Выпуск билета: payload + HMAC в @p out (kTicketLen байт). */
inline bool issue(const uint8_t *key, size_t keyLen,
                  uint32_t clientIp, uint32_t nowMs, uint32_t ttlMs, uint32_t nonce,
                  char *out, size_t outLen) {
    if (!out || outLen < kTicketLen || !key || keyLen == 0) return false;
    char payload[kPayloadLen + 1];
    const uint32_t expiry = nowMs + ttlMs;
    if (!formatPayload(payload, sizeof(payload), expiry, clientIp, nonce)) {
        out[0] = '\0';
        return false;
    }
    if (!hmacSha256Hex(key, keyLen, payload, out + kPayloadLen, outLen - kPayloadLen)) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, payload, kPayloadLen);
    out[kTicketLen - 1] = '\0';
    return true;
}

/**
 * @brief Проверка билета: версия, MAC, IP, expiry.
 * @param used  Если не nullptr — consume nonce (одноразовость до expiry).
 * @return false при replay, истечении TTL или неверном MAC/IP.
 */
inline bool verify(const uint8_t *key, size_t keyLen,
                   uint32_t clientIp, uint32_t nowMs, const char *ticket,
                   NonceStore *used = nullptr) {
    if (!key || !ticket) return false;
    if (strnlen(ticket, kTicketLen) != kTicketLen - 1) return false;
    if (ticket[0] != kVersion) return false;

    char payload[kPayloadLen + 1];
    memcpy(payload, ticket, kPayloadLen);
    payload[kPayloadLen] = '\0';

    char expected[kMacHexLen + 1];
    if (!hmacSha256Hex(key, keyLen, payload, expected, sizeof(expected))) {
        return false;
    }
    if (!constTimeHexEq(expected, ticket + kPayloadLen, kMacHexLen)) {
        return false;
    }

    unsigned long expiry = 0, boundIp = 0, nonce = 0;
    if (sscanf(payload + 1, "%08lx%08lx%08lx", &expiry, &boundIp, &nonce) != 3) {
        return false;
    }
    if ((uint32_t)boundIp != clientIp) return false;
    if ((int32_t)(nowMs - (uint32_t)expiry) >= 0) return false;
    if (used && !used->consume((uint32_t)nonce, (uint32_t)expiry, nowMs)) {
        return false;
    }
    return true;
}

}  // namespace WsTicketAuth

#endif  // WS_TICKET_AUTH_H
