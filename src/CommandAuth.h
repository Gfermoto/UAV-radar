/**
 * @file    CommandAuth.h
 * @brief   HMAC-SHA256 для MQTT-команд (host-тестируемые pure helpers).
 *
 * ## Форматы payload (buildSignedPayload)
 *
 * legacy (пустой nonce): `cmd|ts|` + compact JSON(value)
 * v2 (nonce не пуст):     `cmd|ts|<nonce>|` + compact JSON(value)
 *
 * hmacHex / verifyHmac — HMAC-SHA256 → 64 hex; constTimeHexEq — сравнение digest.
 *
 * @see MQTTManager.h, docs/API_REFERENCE.md, docs/ARCHITECTURE.md
 */
#ifndef COMMAND_AUTH_H
#define COMMAND_AUTH_H

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace CommandAuth {

/** Constant-time compare of two hex digests (length n, typically 64) + NUL. */
inline bool constTimeHexEq(const char *a, const char *b, size_t n) {
    if (!a || !b) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    diff |= (uint8_t)a[n] ^ (uint8_t)b[n];  // both must be NUL-terminated
    return diff == 0;
}

/**
 * Build canonical MAC payload into out.
 * legacy (пустой nonce): cmd|ts| + compact JSON(value)
 * v2 (nonce не пуст): cmd|ts|<nonce>| + compact JSON(value)
 * @return bytes written (strlen), 0 on error
 */
inline size_t buildSignedPayload(char *out, size_t outLen, const char *cmd,
                                 int64_t ts, const char *nonce,
                                 JsonVariantConst val) {
    if (!out || outLen < 8 || !cmd || !cmd[0] || ts <= 0) return 0;
    const bool hasNonce = (nonce && nonce[0]);
    int n;
    if (hasNonce) {
        n = snprintf(out, outLen, "%s|%lld|%s|", cmd, (long long)ts, nonce);
    } else {
        n = snprintf(out, outLen, "%s|%lld|", cmd, (long long)ts);
    }
    if (n <= 0 || (size_t)n >= outLen) return 0;
    if (!val.isNull()) {
        size_t base = (size_t)n;
        size_t wrote = serializeJson(val, out + base, outLen - base);
        if (wrote == 0 && !val.isNull()) return 0;
        return base + wrote;
    }
    return (size_t)n;
}

}  // namespace CommandAuth

#include <cstdint>

#if defined(UNIT_TEST)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#else
#include <mbedtls/md.h>
#endif

namespace CommandAuth {

/**
 * Compute HMAC-SHA256 hex string for a payload.
 * Returns the 64-char hex string in @p outHex, or "" on error.
 */
inline bool hmacHex(const uint8_t *key, size_t keyLen,
                    const char *payload, char *outHex, size_t outHexLen) {
    if (!key || !payload || !outHex || outHexLen < 65) return false;
    uint8_t mac[32];
#if defined(UNIT_TEST)
    unsigned int macLen = 0;
    if (!HMAC(EVP_sha256(), key, (int)keyLen,
              (const unsigned char *)payload, strlen(payload),
              mac, &macLen) || macLen < 32) {
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
    for (size_t i = 0; i < 32; ++i) {
        snprintf(outHex + i * 2, 3, "%02x", mac[i]);
    }
    outHex[64] = '\0';
    return true;
}

/**
 * Verify HMAC signature against payload + key.
 * Equivalent to MQTTManager::verifyCommandAuth HMAC step.
 */
inline bool verifyHmac(const uint8_t *key, size_t keyLen,
                       const char *payload, const char *sigHex) {
    if (!key || !payload || !sigHex || strlen(sigHex) != 64) return false;
    char expected[65];
    if (!hmacHex(key, keyLen, payload, expected, sizeof(expected))) return false;
    return constTimeHexEq(sigHex, expected, 64);
}

}  // namespace CommandAuth
#endif  // COMMAND_AUTH_H
