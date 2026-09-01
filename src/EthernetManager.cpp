/**
 * @file    EthernetManager.cpp
 * @brief   Реализация менеджера Ethernet W5500. Только Pro.
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "EthernetManager.h"

#if ETHERNET_ENABLED

#include <Ethernet.h>

/** Интервал проверки состояния (мс). */
#define ETH_CHECK_INTERVAL_MS  5000

/** Таймаут DHCP (мс). */
#define ETH_DHCP_TIMEOUT_MS    10000

EthernetManager::EthernetManager()
    : _initialized(false)
    , _hasIP(false)
    , _lastCheckMs(0)
    , _dhcpBackoff(5000, 60000)
{
    memset(_mac, 0, sizeof(_mac));
}

bool EthernetManager::begin() {
    pinMode(PIN_ETH_RST, OUTPUT);
    pinMode(PIN_ETH_CS, OUTPUT);

    resetChip();

    // Инициализация SPI
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_ETH_CS);
        SPI.setFrequency(ETH_SPI_FREQUENCY);  // Безопасная частота для W5500

    // Генерация MAC из серийного номера ESP32
    uint64_t chipId = 0;
    esp_efuse_mac_get_default((uint8_t*)&chipId);
    // Локально администрируемый MAC (бит 0x02) — не конфликтовать с eFuse WiFi.
    _mac[0] = 0x02;
    _mac[1] = 0x00;
    _mac[2] = (chipId >> 40) & 0xFF;
    _mac[3] = (chipId >> 32) & 0xFF;
    _mac[4] = (chipId >> 24) & 0xFF;
    _mac[5] = (chipId >> 16) & 0xFF;

    Serial.printf("[ETH] Инициализация W5500... MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);

    Ethernet.init(PIN_ETH_CS);

    if (!checkLinkStatus()) {
        Serial.printf("[ETH] Кабель не подключён. Ethernet в standby.\n");
        _initialized = true;
        _hasIP = false;
        return false;
    }

    Serial.printf("[ETH] Кабель подключён. Запрос DHCP...\n");

    if (Ethernet.begin(_mac, ETH_DHCP_TIMEOUT_MS) == 0) {
        Serial.printf("[ETH] Ошибка DHCP\n");
        _initialized = true;
        _hasIP = false;
        return false;
    }

    _hasIP = true;
    _initialized = true;

    Serial.printf("[ETH] Подключён! IP: %s, Шлюз: %s, DNS: %s\n",
                  Ethernet.localIP().toString().c_str(),
                  Ethernet.gatewayIP().toString().c_str(),
                  Ethernet.dnsServerIP().toString().c_str());
    return true;
}

void EthernetManager::end() {
    // W5500 не имеет прямого метода shutdown в Arduino API;
    // просто помечаем как не инициализированный.
    _initialized = false;
    _hasIP = false;
    Serial.printf("[ETH] Отключён\n");
}

bool EthernetManager::isConnected() const {
    return _initialized && _hasIP && Ethernet.linkStatus() == LinkON;
}

bool EthernetManager::isLinkUp() const {
    return _initialized && Ethernet.linkStatus() == LinkON;
}

IPAddress EthernetManager::getLocalIP() const {
    if (_initialized && _hasIP) {
        return Ethernet.localIP();
    }
    return IPAddress(0, 0, 0, 0);
}

void EthernetManager::update() {
    if (!_initialized) return;

    uint32_t now = millis();
    if (now - _lastCheckMs < ETH_CHECK_INTERVAL_MS) return;
    _lastCheckMs = now;

    bool linkUp = checkLinkStatus();

    if (linkUp && !_hasIP) {
        const uint32_t now = millis();
        // DHCP backoff: не спамить begin() при флапающем линке.
        if (!_dhcpBackoff.ready(now)) return;
        Serial.printf("[ETH] Кабель подключён. Переподключение DHCP...\n");
        if (Ethernet.begin(_mac, ETH_DHCP_TIMEOUT_MS) != 0) {
            _hasIP = true;
            _dhcpBackoff.recordSuccess();
            Serial.printf("[ETH] Восстановлен! IP: %s\n",
                          Ethernet.localIP().toString().c_str());
        } else {
            Serial.printf("[ETH] DHCP не удался\n");
            _dhcpBackoff.recordFailure(now);
        }
    } else if (!linkUp && _hasIP) {
        Serial.printf("[ETH] Кабель отключён\n");
        _hasIP = false;
    }
}

// =============================================================================
//  Приватные методы
// =============================================================================

void EthernetManager::resetChip() {
    digitalWrite(PIN_ETH_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(PIN_ETH_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));  // Ждём стабилизации W5500
    Serial.printf("[ETH] W5500 сброшен\n");
}

bool EthernetManager::checkLinkStatus() {
    return (Ethernet.linkStatus() == LinkON);
}

#endif // ETHERNET_ENABLED
