/**
 * @file    TlsUtil.cpp
 */

#include "TlsUtil.h"
#include <Preferences.h>
#include <cstring>

namespace TlsUtil {

// WiFiClientSecure::setCACert stores the pointer without copying.
// Fixed buffer: never realloc (realloc would invalidate live client ptrs).
static constexpr size_t kCaMax = 8192;
static char s_caPem[kCaMax];
static size_t s_caLen = 0;

bool configure(WiFiClientSecure &client) {
    Preferences prefs;
    prefs.begin("rtspmic", true);
    String ca = prefs.getString("tls_ca", "");
    prefs.end();

    if (ca.length() <= 64 || ca.indexOf("BEGIN CERTIFICATE") < 0) {
        // Не паникуем — у DIY может не быть tls_ca; вызывающий пусть пробует PROGMEM.
        // Serial.printf здесь не логируем — слишком много шума.
        return false;
    }

    const size_t need = (size_t)ca.length() + 1;
    if (need > kCaMax) {
        Serial.printf("[TLS] ERROR: tls_ca too large (%u > %u)\n",
                      (unsigned)need, (unsigned)kCaMax);
        return false;
    }

    memcpy(s_caPem, ca.c_str(), need);
    s_caLen = need;
    client.setCACert(s_caPem);
    Serial.printf("[TLS] CA loaded from NVS (%u bytes)\n", (unsigned)ca.length());
    return true;
}

void invalidate() {
    // Callers must stop/invalidate TLS clients first (setCACert stores a ptr).
    // Zero only after clients dropped the connection; leave a valid C-string.
    if (s_caLen > 0) {
        memset(s_caPem, 0, s_caLen);
        s_caLen = 0;
    }
    s_caPem[0] = '\0';
}

}  // namespace TlsUtil
