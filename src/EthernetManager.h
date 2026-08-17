/**
 * @file    EthernetManager.h
 * @brief   Опциональный Ethernet через W5500 (SPI). Core 0.
 *
 * ## Назначение
 *
 * Проводной интерфейс параллельно Wi‑Fi. Включается при `ETHERNET_ENABLED=1`.
 * W5500 на SPI: CS=GPIO10, INT=GPIO11, RST=GPIO12, SCK=13, MOSI=15, MISO=14.
 *
 * ## Жизненный цикл W5500
 *
 *   1. `begin()` — reset чипа, инициализация SPI, DHCP, MAC из eFuse
 *   2. `update()` — периодическая проверка link/IP (`checkLinkStatus`), backoff при DHCP fail
 *   3. `isLinkUp()` / `isConnected()` — физический линк и наличие IP
 *   4. `end()` — деинициализация, освобождение SPI
 *
 * `RtpStreamGuard` / `ReconnectBackoff` — согласованное восстановление после обрыва линка.
 *
 * @see WiFiSetup, docs/ARCHITECTURE.md § «Сеть и UI»
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef ETHERNET_MANAGER_H
#define ETHERNET_MANAGER_H

#include <Arduino.h>
#include "Config.h"

#if ETHERNET_ENABLED
#include <SPI.h>
#include "RtpStreamGuard.h"

class EthernetManager {
public:
    EthernetManager();

    /**
     * @brief Инициализация W5500, DHCP, запуск мониторинга.
     * @return false при ошибке reset/SPI/DHCP.
     */
    bool begin();

    /** @brief Остановка Ethernet и освобождение ресурсов. */
    void end();

    /** @return true если получен IP (DHCP/static). */
    bool isConnected() const;

    /** @return true если кабель подключён (link up). */
    bool isLinkUp() const;

    /** @return Текущий локальный IPv4. */
    IPAddress getLocalIP() const;

    /**
     * @brief Периодический poll: link, DHCP renew, backoff.
     * Вызывать из Core 0 loop (не из audio task).
     */
    void update();

private:
    bool    _initialized;
    bool    _hasIP;
    uint8_t _mac[6];
    uint32_t _lastCheckMs;
    ReconnectBackoff _dhcpBackoff;

    /** Аппаратный reset W5500 (RST pin). */
    void resetChip();

    /** Чтение регистра link и обновление _hasIP. */
    bool checkLinkStatus();
};

#endif // ETHERNET_ENABLED

#endif // ETHERNET_MANAGER_H
