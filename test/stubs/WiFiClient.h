#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IPAddress {
public:
    IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0)
        : _a(a), _b(b), _c(c), _d(d) {}
    String toString() const { return String("192.168.1.50"); }
private:
    uint8_t _a, _b, _c, _d;
};

namespace FakeWifi {

struct Socket {
    std::mutex mutex;
    bool connected = false;
    std::deque<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
    size_t maximumWrite = std::numeric_limits<size_t>::max();
};

struct Runtime {
    std::mutex mutex;
    bool connectResult = false;
    uint32_t connectAdvanceMs = 0;
    std::vector<uint32_t> connectAttemptTimes;
    std::deque<std::shared_ptr<Socket>> pendingServerClients;
    std::atomic<uint32_t> networkCallsUnderMutex{0};
};

inline Runtime runtime;

inline void recordNetworkCall() {
    if (fake_freertos_mutex_held_by_current_thread()) {
        runtime.networkCallsUnderMutex.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace FakeWifi

class WiFiClient {
public:
    WiFiClient() : _socket(std::make_shared<FakeWifi::Socket>()) {}

    bool connect(const char *, uint16_t) { return connect(nullptr, 0, 0); }
    bool connect(const char *, uint16_t, uint32_t) {
        FakeWifi::recordNetworkCall();
        bool result;
        uint32_t advance;
        {
            std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
            FakeWifi::runtime.connectAttemptTimes.push_back(millis());
            result = FakeWifi::runtime.connectResult;
            advance = FakeWifi::runtime.connectAdvanceMs;
        }
        test_millis_advance(advance);
        std::lock_guard<std::mutex> lock(_socket->mutex);
        _socket->connected = result;
        return result;
    }

    void stop() {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(_socket->mutex);
        _socket->connected = false;
    }

    bool connected() const {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(_socket->mutex);
        return _socket->connected;
    }

    explicit operator bool() const { return connected(); }

    size_t write(const uint8_t *data, size_t size) {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(_socket->mutex);
        if (!_socket->connected) return 0;
        const size_t count = std::min(size, _socket->maximumWrite);
        _socket->outgoing.insert(_socket->outgoing.end(), data, data + count);
        return count;
    }

    size_t available() const {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(_socket->mutex);
        return _socket->incoming.size();
    }

    int read() {
        FakeWifi::recordNetworkCall();
        std::lock_guard<std::mutex> lock(_socket->mutex);
        if (_socket->incoming.empty()) return -1;
        const int value = _socket->incoming.front();
        _socket->incoming.pop_front();
        return value;
    }

    size_t printf(const char *format, ...) {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        if (length <= 0) return 0;
        return write(reinterpret_cast<const uint8_t *>(buffer),
                     std::min(static_cast<size_t>(length), sizeof(buffer) - 1));
    }

    size_t print(const char *text) {
        if (!text) return 0;
        return write(reinterpret_cast<const uint8_t *>(text), std::strlen(text));
    }

    void flush() { FakeWifi::recordNetworkCall(); }
    void setNoDelay(bool) {}
    IPAddress remoteIP() const { return IPAddress(192, 168, 1, 50); }

    void appendIncoming(const char *text) {
        if (!text) return;
        std::lock_guard<std::mutex> lock(_socket->mutex);
        while (*text) _socket->incoming.push_back(static_cast<uint8_t>(*text++));
    }

private:
    explicit WiFiClient(std::shared_ptr<FakeWifi::Socket> socket)
        : _socket(std::move(socket)) {}
    std::shared_ptr<FakeWifi::Socket> _socket;

    friend WiFiClient fake_wifi_make_client(const char *);
    friend void fake_wifi_server_enqueue(const WiFiClient &);
    friend class WiFiServer;
};

inline void fake_wifi_reset() {
    std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
    FakeWifi::runtime.connectResult = false;
    FakeWifi::runtime.connectAdvanceMs = 0;
    FakeWifi::runtime.connectAttemptTimes.clear();
    FakeWifi::runtime.pendingServerClients.clear();
    FakeWifi::runtime.networkCallsUnderMutex.store(0, std::memory_order_relaxed);
}

inline void fake_wifi_set_connect_result(bool result, uint32_t advanceMs) {
    std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
    FakeWifi::runtime.connectResult = result;
    FakeWifi::runtime.connectAdvanceMs = advanceMs;
}

inline size_t fake_wifi_connect_attempt_count() {
    std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
    return FakeWifi::runtime.connectAttemptTimes.size();
}

inline uint32_t fake_wifi_connect_attempt_time(size_t index) {
    std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
    return index < FakeWifi::runtime.connectAttemptTimes.size()
               ? FakeWifi::runtime.connectAttemptTimes[index]
               : 0;
}

inline WiFiClient fake_wifi_make_client(const char *incoming) {
    auto socket = std::make_shared<FakeWifi::Socket>();
    socket->connected = true;
    WiFiClient client(socket);
    client.appendIncoming(incoming);
    return client;
}

inline void fake_wifi_server_enqueue(const WiFiClient &client) {
    std::lock_guard<std::mutex> lock(FakeWifi::runtime.mutex);
    FakeWifi::runtime.pendingServerClients.push_back(client._socket);
}

inline uint32_t fake_wifi_network_calls_under_mutex() {
    return FakeWifi::runtime.networkCallsUnderMutex.load(std::memory_order_relaxed);
}
