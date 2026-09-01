/**
 * @file DiagnosticsNvs.h
 * @brief NVS-хелперы диагностики загрузки (last_event / last_error).
 *
 * Namespace: "rtspmic". Не трогает NetConfig / WiFi / credentials keys.
 * writeLastEvent/writeLastError — перед reboot/crash; readBoot — при старте.
 *
 * @see SystemMonitor.h, docs/ARCHITECTURE.md
 */
#ifndef DIAGNOSTICS_NVS_H
#define DIAGNOSTICS_NVS_H

#include <Arduino.h>
#include <Preferences.h>

namespace DiagnosticsNvs {

constexpr const char *kNamespace = "rtspmic";
constexpr const char *kLastEvent = "last_event";
constexpr const char *kLastError = "last_error";

struct BootInfo {
    String lastEvent;
    String lastError;
};

inline BootInfo readBoot() {
    BootInfo info;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return info;
    info.lastEvent = prefs.getString(kLastEvent, "");
    info.lastError = prefs.getString(kLastError, "");
    prefs.end();
    return info;
}

inline bool writeLastEvent(const char *event, bool withEventTime = false) {
    if (!event || !event[0]) return false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const bool ok = prefs.putString(kLastEvent, event) > 0;
    if (withEventTime) {
        prefs.putUInt("event_time", millis());
    }
    prefs.end();
    return ok;
}

inline bool writeLastError(const char *error) {
    if (!error || !error[0]) return false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const bool ok = prefs.putString(kLastError, error) > 0;
    prefs.end();
    return ok;
}

inline void clearBootKeys() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.remove(kLastEvent);
    prefs.remove(kLastError);
    prefs.end();
}

}  // namespace DiagnosticsNvs

#endif  // DIAGNOSTICS_NVS_H
