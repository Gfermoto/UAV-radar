#pragma once

#include <stdint.h>
#include <Arduino.h>
#include "WiFiClient.h"

class WiFiClass {
public:
    int status() { return 3; }
    void disconnect() {}
    void reconnect() {}
    void macAddress(uint8_t mac[6]) {
        mac[0] = 0xAA; mac[1] = 0xBB; mac[2] = 0xCC;
        mac[3] = 0x01; mac[4] = 0x02; mac[5] = 0x03;
    }
    String macAddress() { return String("AA:BB:CC:01:02:03"); }
    const char* SSID() { return "test-ssid"; }
    IPAddress localIP() { return IPAddress(192, 168, 1, 50); }
    int RSSI() { return -55; }
};

extern WiFiClass WiFi;
#define WL_CONNECTED 3

class WiFiServer {
public:
    WiFiServer(uint16_t, uint8_t = 4) {}
    void begin() { FakeWifi::recordNetworkCall(); }
    void stop() { FakeWifi::recordNetworkCall(); }

    bool hasClient() {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
        return !FakeWifi::runtime.pendingServerClients.empty();
    }

    WiFiClient accept() {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
        if (FakeWifi::runtime.pendingServerClients.empty()) return WiFiClient();
        auto socket = FakeWifi::runtime.pendingServerClients.front();
        FakeWifi::runtime.pendingServerClients.pop_front();
        return WiFiClient(socket);
    }
};

class ESPClass {
public:
    void restart() {}
    uint32_t getFreeHeap() { return 200000; }
    uint32_t getHeapSize() { return 320000; }
    uint32_t getFreePsram() { return 4000000; }
    uint32_t getMinFreeHeap() { return 180000; }  ///< native stub watermark
    uint32_t getCpuFreqMHz() { return 240; }
};

extern ESPClass ESP;
