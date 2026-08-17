/**
 * @file    WiFiSetup.h
 * @brief   Настройка Wi-Fi: captive portal, NVS-credentials, переподключение.
 *
 * ## Назначение
 *
 * Обеспечивает удобную настройку Wi-Fi на IoT-устройстве без экрана
 * и кнопок. Использует паттерн captive portal (как в умных розетках,
 * камерах, ESPHome):
 *
 *   1. При первом запуске (нет credentials в NVS) — создаёт точку доступа
 *      `RTSPMIC-Setup` с IP `192.168.4.1`
 *   2. Пользователь подключается к AP, открывает браузер — автоматически
 *      попадает на страницу настройки через DNS-перехват
 *   3. Выбирает Wi-Fi сеть (автосканирование), вводит пароль, API-ключ
 *   4. Данные сохраняются в NVS, устройство переподключается
 *
 * ## Использование
 *
 * @code
 *   // В setup() — однократный вызов:
 *   if (!WiFiSetup::ensureConnection()) {
 *       Serial.println("No WiFi — offline mode");
 *   }
 *
 *   // В loop() — для обработки DNS captive portal:
 *   WiFiSetup::processDNS();
 * @endcode
 *
 * ## Captive Portal
 *
 * Для совместимости со всеми ОС реализованы стандартные эндпоинты:
 *   - `/generate_204` — Android
 *   - `/hotspot-detect.html`, `/library/test/success.html` — Apple
 *   - `/ncsi.txt`, `/connecttest.txt` — Windows
 *   - `/fwlink` — Microsoft
 *
 * Все они перенаправляют на `/` — страницу настройки Wi-Fi.
 *
 * ## Хранение данных
 *
 * Preferences (NVS), namespace `rtspmic`:
 *   - `wifi_ssid` — SSID сети
 *   - `wifi_pass` — пароль
 *   - `api_key`  — ключ API (опционально, задаётся на портале)
 *
 * Сброс credentials: `WiFiSetup::resetCredentials()` или
 * `/api/system/factory_reset` через WebUI.
 *
 * @see docs/ARCHITECTURE.md § «Сеть и UI»
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "Config.h"

/**
 * @brief Статический класс для настройки и управления Wi-Fi.
 *
 * Все методы статические (singleton-паттерн через статические поля).
 * Это позволяет вызывать их из любого модуля без передачи экземпляра.
 *
 * @note Оборачивает ESPAsyncWebServer и DNSServer для captive portal.
 *       После успешного подключения эти объекты удаляются для экономии памяти.
 */
class WiFiSetup {
public:
    /**
     * @brief Главная точка входа — гарантирует подключение к Wi-Fi.
     *
     * Алгоритм:
     *   1. Проверяет NVS на сохранённые credentials
     *   2. Если есть — пытается подключиться
     *   3. Если нет или не удалось — запускает captive portal
     *   4. Ждёт подключения в течение AP_PORTAL_TIMEOUT_MS (180 сек)
     *   5. При таймауте — перезагружает устройство
     *
     * @return true если подключение установлено.
     */
    static bool ensureConnection();

    /** @brief Удаление сохранённых credentials из NVS. */
    static void resetCredentials();

    /** @brief Инициализация deviceId/hostname из MAC (NV%02X…). Вызывать до AP/STA. */
    static void initDeviceIdentity();

    /** @return Идентификатор узла = пароль AP по умолчанию, напр. "NVD4746E8C". */
    static const char *deviceId();

    /** @return mDNS/DHCP hostname (lowercase deviceId), напр. "nvd4746e8c". */
    static const char *hostname();

    /** @return true если в NVS есть сохранённый SSID. */
    static bool hasCredentials();

    /** @return true если активен режим настройки (captive portal). */
    static bool isConfigMode();

    /** @brief Запуск captive portal: AP + DNS + веб-сервер. */
    static void startConfigPortalAsync();

    /** @brief Остановка captive portal и освобождение ресурсов. */
    static void stopConfigPortal();

    /**
     * @brief Обработка DNS-запросов captive portal.
     *
     * Должен вызываться регулярно (каждый tick) в loop() для
     * перенаправления DNS на IP точки доступа.
     */
    static void processDNS();

    /** @brief BOOT (GPIO0): hold ≥ FACTORY_RESET_HOLD_MS → wipe NVS + reboot (STA и portal). */
    static void pollFactoryResetButton();

    /**
     * @brief Полный сброс STA + повторное подключение (не "залипший" reconnect).
     *        disconnect(true) → mode(WIFI_STA) → setSleep(false) → autoReconnect → begin.
     */
    static void fullStaResetAndReconnect();

    /** @brief Настройка STA без modem-sleep + auto-reconnect (для tryConnect / recovery). */
    static void configureStaNoSleep();

    /**
     * @brief Доступ к веб-серверу портала.
     * @return Указатель на AsyncWebServer (nullptr если портал неактивен).
     */
    static AsyncWebServer* getServer();

private:
    // ── Ключи NVS ──
    static const char* NVS_NS;   ///< "rtspmic"
    static const char* KEY_SSID; ///< "wifi_ssid"
    static const char* KEY_PASS; ///< "wifi_pass"
    // AP SSID = deviceId() (тот же NV+MAC, что и пароль / hostname)

    // ── Объекты captive portal ──
    static DNSServer      *_dns;         ///< DNS-сервер (перенаправление)
    static AsyncWebServer *_server;      ///< Веб-сервер портала
    static bool            _configMode;  ///< Флаг активности портала
    static uint32_t        _portalStartMs; ///< Время запуска портала (мс)

    // ── Внутренние методы ──

    /** @brief Чтение SSID/пароля из NVS. */
    static bool loadCredentials(String &ssid, String &pass);

    /** @brief Сохранение SSID/пароля в NVS. */
    static void saveCredentials(const String &ssid, const String &pass);

    /** @brief Создание пароля AP из MAC-адреса. */
    static String buildApPassword();

    /**
     * @brief Попытка подключения к Wi-Fi с повторами.
     * @param ssid      Имя сети.
     * @param pass      Пароль.
     * @param timeoutMs Таймаут одной попытки (мс).
     * @return true если подключение установлено.
     */
    static bool tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs);

    /** @brief Регистрация маршрутов captive portal. */
    static void setupPortalRoutes();

    /** @return HTML-страница настройки Wi-Fi. */
    static String buildPortalHtml();

    /** Setup token — проверяется в POST /save captive portal. */
    static char s_setupToken[9];

    /** NV+MAC identity (SSID AP / пароль AP / отображаемое имя). */
    static char s_deviceId[DEVICE_ID_LEN];
    /** Lowercase deviceId for mDNS / DHCP hostname. */
    static char s_hostname[DEVICE_ID_LEN];

    /** Отложенное подключение после /save (чтобы успеть отправить HTTP 200). */
    static volatile bool s_pendingConnect;
    static String        s_pendingSsid;
    static String        s_pendingPass;
};

#endif