#ifndef PUBSUBCLIENT_H
#define PUBSUBCLIENT_H

#include <Arduino.h>
#include <functional>
#include <cstring>

class WiFiClient;

using MQTT_CALLBACK_SIGNATURE = std::function<void(char *, uint8_t *, unsigned int)>;

class PubSubClient {
public:
    PubSubClient() {}
    explicit PubSubClient(WiFiClient &) {}

    void setServer(const char *, uint16_t) {}
    void setCallback(MQTT_CALLBACK_SIGNATURE) {}
    void setKeepAlive(uint16_t) {}
    void setBufferSize(uint16_t) {}

    bool connect(const char *) { return true; }
    bool connect(const char *, const char *, const char *) { return true; }
    bool connect(const char *, const char *, int, bool, const char *) { return true; }  // LWT overload
    bool connect(const char *, const char *, const char *, const char *, int, bool, const char *) { return true; }
    void disconnect() {}
    bool connected() { return false; }
    bool publish(const char *, const char *) { return true; }
    bool publish(const char *, const char *, bool) { return true; }
    bool publish(const char *, const uint8_t *, unsigned int) { return true; }
    bool subscribe(const char *) { return true; }
    bool loop() { return false; }
    int state() { return 0; }
};

#endif
