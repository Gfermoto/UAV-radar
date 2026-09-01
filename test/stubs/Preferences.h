#pragma once

#include <stdint.h>
#include <string>
#include <map>
#include <cstring>
#include "WString.h"

/** Global flag to simulate NVS write failure in tests. */
extern bool g_prefs_write_fail;

/** Forward declaration: shared NVS storage is defined in stubs_globals.cpp. */
void nvs_clear();
bool nvs_get_bool(const std::string &key);
void nvs_put_bool(const std::string &key, bool v);
int8_t nvs_get_char(const std::string &key);
void nvs_put_char(const std::string &key, int8_t v);
uint8_t nvs_get_uchar(const std::string &key);
void nvs_put_uchar(const std::string &key, uint8_t v);
int16_t nvs_get_short(const std::string &key);
void nvs_put_short(const std::string &key, int16_t v);
uint16_t nvs_get_ushort(const std::string &key);
void nvs_put_ushort(const std::string &key, uint16_t v);
int32_t nvs_get_int(const std::string &key);
void nvs_put_int(const std::string &key, int32_t v);
uint32_t nvs_get_uint(const std::string &key);
void nvs_put_uint(const std::string &key, uint32_t v);
float nvs_get_float(const std::string &key);
void nvs_put_float(const std::string &key, float v);
double nvs_get_double(const std::string &key);
void nvs_put_double(const std::string &key, double v);
std::string nvs_get_string(const std::string &key);
void nvs_put_string(const std::string &key, const std::string &v);
bool nvs_has_key(const std::string &key);
void nvs_remove(const std::string &key);

class Preferences {
public:
    bool begin(const char *ns, bool readOnly = false) {
        ns_ = ns ? ns : "";
        readOnly_ = readOnly;
        beginOk_ = true;
        return true;
    }
    void end() {}

    bool putBool(const char *key, bool v) {
        if (g_prefs_write_fail) return false;
        nvs_put_bool(keyStr(key), v);
        return true;
    }
    bool getBool(const char *key, bool def = false) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_bool(k) : def;
    }

    bool putChar(const char *key, int8_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_char(keyStr(key), v);
        return true;
    }
    int8_t getChar(const char *key, int8_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_char(k) : def;
    }

    bool putUChar(const char *key, uint8_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_uchar(keyStr(key), v);
        return true;
    }
    uint8_t getUChar(const char *key, uint8_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_uchar(k) : def;
    }

    bool putShort(const char *key, int16_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_short(keyStr(key), v);
        return true;
    }
    int16_t getShort(const char *key, int16_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_short(k) : def;
    }

    bool putUShort(const char *key, uint16_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_ushort(keyStr(key), v);
        return true;
    }
    uint16_t getUShort(const char *key, uint16_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_ushort(k) : def;
    }

    bool putInt(const char *key, int32_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_int(keyStr(key), v);
        return true;
    }
    int32_t getInt(const char *key, int32_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_int(k) : def;
    }

    bool putUInt(const char *key, uint32_t v) {
        if (g_prefs_write_fail) return false;
        nvs_put_uint(keyStr(key), v);
        return true;
    }
    uint32_t getUInt(const char *key, uint32_t def = 0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_uint(k) : def;
    }

    bool putLong(const char *key, int32_t v) { return putInt(key, v); }
    int32_t getLong(const char *key, int32_t def = 0) { return getInt(key, def); }

    bool putULong(const char *key, uint32_t v) { return putUInt(key, v); }
    uint32_t getULong(const char *key, uint32_t def = 0) { return getUInt(key, def); }

    bool putFloat(const char *key, float v) {
        if (g_prefs_write_fail) return false;
        nvs_put_float(keyStr(key), v);
        return true;
    }
    float getFloat(const char *key, float def = 0.0f) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_float(k) : def;
    }

    bool putDouble(const char *key, double v) {
        if (g_prefs_write_fail) return false;
        nvs_put_double(keyStr(key), v);
        return true;
    }
    double getDouble(const char *key, double def = 0.0) {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? nvs_get_double(k) : def;
    }

    bool putString(const char *key, const char *v) {
        if (g_prefs_write_fail) return false;
        nvs_put_string(keyStr(key), v ? v : "");
        return true;
    }
    String getString(const char *key, const char *def = "") {
        std::string k = keyStr(key);
        return nvs_has_key(k) ? String(nvs_get_string(k).c_str()) : String(def);
    }

    bool isKey(const char *key) {
        return nvs_has_key(keyStr(key));
    }

    bool remove(const char *key) {
        nvs_remove(keyStr(key));
        return true;
    }

    bool clear() {
        nvs_clear();
        return true;
    }

    size_t freeEntries() { return 100; }

private:
    std::string keyStr(const char *k) {
        return ns_ + "/" + (k ? k : "");
    }

    std::string ns_;
    bool readOnly_ = false;
    bool beginOk_ = false;
};