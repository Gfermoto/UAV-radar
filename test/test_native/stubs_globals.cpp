/**
 * @file    stubs_globals.cpp
 * @brief   ESP/WiFi globals и in-memory NVS (Preferences) для native test harness.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <map>
#include <string>

ESPClass ESP;

/** Global flag to simulate NVS write failure in tests. */
bool g_prefs_write_fail = false;

// ── Shared NVS storage (all Preferences instances share the same maps) ──

static std::map<std::string, bool>      s_bools;
static std::map<std::string, int8_t>    s_chars;
static std::map<std::string, uint8_t>   s_uchars;
static std::map<std::string, int16_t>   s_shorts;
static std::map<std::string, uint16_t>  s_ushorts;
static std::map<std::string, int32_t>   s_ints;
static std::map<std::string, uint32_t>  s_uints;
static std::map<std::string, float>     s_floats;
static std::map<std::string, double>    s_doubles;
static std::map<std::string, std::string> s_strings;

void nvs_clear() {
    s_bools.clear(); s_chars.clear(); s_uchars.clear();
    s_shorts.clear(); s_ushorts.clear();
    s_ints.clear(); s_uints.clear();
    s_floats.clear(); s_doubles.clear();
    s_strings.clear();
}

bool nvs_has_key(const std::string &key) {
    return s_bools.count(key) || s_chars.count(key) || s_uchars.count(key) ||
           s_shorts.count(key) || s_ushorts.count(key) || s_ints.count(key) ||
           s_uints.count(key) || s_floats.count(key) || s_doubles.count(key) ||
           s_strings.count(key);
}

void nvs_remove(const std::string &key) {
    s_bools.erase(key); s_chars.erase(key); s_uchars.erase(key);
    s_shorts.erase(key); s_ushorts.erase(key);
    s_ints.erase(key); s_uints.erase(key);
    s_floats.erase(key); s_doubles.erase(key);
    s_strings.erase(key);
}

#define NVS_IMPL(T, name, map) \
    T nvs_get_##name(const std::string &key) { auto it = map.find(key); return it == map.end() ? T() : it->second; } \
    void nvs_put_##name(const std::string &key, T v) { map[key] = v; }

NVS_IMPL(bool, bool, s_bools)
NVS_IMPL(int8_t, char, s_chars)
NVS_IMPL(uint8_t, uchar, s_uchars)
NVS_IMPL(int16_t, short, s_shorts)
NVS_IMPL(uint16_t, ushort, s_ushorts)
NVS_IMPL(int32_t, int, s_ints)
NVS_IMPL(uint32_t, uint, s_uints)
NVS_IMPL(float, float, s_floats)
NVS_IMPL(double, double, s_doubles)

std::string nvs_get_string(const std::string &key) {
    auto it = s_strings.find(key);
    return it == s_strings.end() ? std::string() : it->second;
}
void nvs_put_string(const std::string &key, const std::string &v) {
    s_strings[key] = v;
}