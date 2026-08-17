/**
 * @file    WiFiSetup.cpp
 * @brief   Реализация captive portal и управления Wi-Fi через NVS.
 *
 * ## Паттерн использования
 *
 * Реализует стандартный IoT-паттерн настройки Wi-Fi:
 *
 *   1. `ensureConnection()` — главная точка входа
 *      - Проверяет NVS на сохранённые credentials
 *      - Если есть — пытается подключиться (с повторами)
 *      - Если нет или не удалось — запускает captive portal
 *
 *   2. Captive portal (192.168.4.1, SSID: RTSPMIC-Setup)
 *      - AP с публичным паролем (только для настройки)
 *      - DNS-сервер перехватывает все запросы → 192.168.4.1
 *      - Веб-интерфейс: сканирование сетей, ввод SSID/пароля/API-ключа
 *      - Совместимость с Android, Apple, Windows (captive portal detection)
 *
 *   3. Сохранение credentials в NVS и переподключение
 *
 * ## Совместимость с ОС (captive portal detection)
 *
 * Каждая ОС использует свой механизм обнаружения captive portal:
 *   - Android: GET /generate_204 → ожидает 204 No Content
 *   - Apple:   GET /hotspot-detect.html → ожидает Success
 *   - Windows: GET /ncsi.txt, /connecttest.txt → ожидает специфический ответ
 *   - Microsoft: GET /fwlink → перенаправление
 *
 * Все эти эндпоинты реализованы для автоматического открытия
 * страницы настройки при подключении к AP.
 *
 * ## Безопасность
 *
 *   - Пароль AP: `NV`+MAC nibbles (или `AP_PASSWORD` если задан) — только для первичной настройки
 *   - Credentials хранятся в NVS (Preferences, namespace "rtspmic")
 *   - API-ключ сохраняется в том же namespace
 *   - Factory reset очищает все данные NVS
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#include "WiFiSetup.h"
#include "Config.h"
#include "DiagnosticsNvs.h"
#include <WiFi.h>
#if ARDUINO_USB_MODE
#include "HWCDC.h"
#endif

// ── Статические константы ──

const char* WiFiSetup::NVS_NS    = "rtspmic";
const char* WiFiSetup::KEY_SSID  = "wifi_ssid";
const char* WiFiSetup::KEY_PASS  = "wifi_pass";

// ── Статические поля ──

DNSServer*      WiFiSetup::_dns           = nullptr;
AsyncWebServer* WiFiSetup::_server        = nullptr;
bool            WiFiSetup::_configMode    = false;
uint32_t        WiFiSetup::_portalStartMs = 0;
char            WiFiSetup::s_setupToken[9] = {};
char            WiFiSetup::s_deviceId[DEVICE_ID_LEN] = {};
char            WiFiSetup::s_hostname[DEVICE_ID_LEN] = {};
volatile bool   WiFiSetup::s_pendingConnect = false;
String          WiFiSetup::s_pendingSsid;
String          WiFiSetup::s_pendingPass;

// ── ensureConnection — главная точка входа ──

void WiFiSetup::configureStaNoSleep() {
    // Надёжность: без modem-sleep (источник "залипаний"), auto-reconnect, hostname.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(hostname());
}

bool WiFiSetup::ensureConnection() {
    String ssid, pass;

    // ── Попытка 1: сохранённые credentials ──
    if (loadCredentials(ssid, pass)) {
        Serial.printf("[WIFI] Found credentials: %s\n", ssid.c_str());
        if (tryConnect(ssid, pass, WIFI_CONNECT_TIMEOUT_MS)) {
            Serial.printf("[WIFI] Connected to %s, IP: %s\n",
                          ssid.c_str(), WiFi.localIP().toString().c_str());
            return true;
        }
        Serial.printf("[WIFI] Saved credentials failed to connect\n");
    } else {
        Serial.printf("[WIFI] No saved credentials in NVS '%s'\n", NVS_NS);
    }

    // ── Попытка 2: captive portal ──
    Serial.printf("[WIFI] Starting captive portal: %s\n", deviceId());
    startConfigPortalAsync();

    // Ожидание настройки через портал (блокирующий цикл с DNS)
    uint32_t start = millis();
    while (millis() - start < AP_PORTAL_TIMEOUT_MS) {
        processDNS();                          // DNS + отложенный connect после /save
        if (WiFi.status() == WL_CONNECTED) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // ── Таймаут / нет связи → перезагрузка ──
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] Portal timeout or connect failed, rebooting...\n");
        DiagnosticsNvs::writeLastError("wifi_portal_timeout");
        ESP.restart();
    }

    return (WiFi.status() == WL_CONNECTED);
}

void WiFiSetup::fullStaResetAndReconnect() {
    String ssid, pass;
    if (!loadCredentials(ssid, pass)) {
        Serial.printf("[WIFI] fullStaReset: no credentials\n");
        return;
    }
    Serial.printf("[WIFI] fullStaReset: disconnect→reinit→begin '%s'\n", ssid.c_str());
    WiFi.disconnect(true);   // wipe=true: сброс driver state, не "залипший" reconnect
    vTaskDelay(pdMS_TO_TICKS(200));
    WiFi.mode(WIFI_STA);
    configureStaNoSleep();
    WiFi.begin(ssid.c_str(), pass.c_str());
}

// ── Управление credentials ──

void WiFiSetup::resetCredentials() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
    prefs.end();
    Serial.printf("[WIFI] Credentials cleared\n");
}

bool WiFiSetup::hasCredentials() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String ssid = prefs.getString(KEY_SSID, "");
    prefs.end();
    return (ssid.length() > 0);
}

bool WiFiSetup::isConfigMode() {
    return _configMode;
}

String WiFiSetup::buildApPassword() {
    if (AP_PASSWORD[0] != '\0') {
        return String(AP_PASSWORD);
    }
    initDeviceIdentity();
    return String(s_deviceId);
}

void WiFiSetup::initDeviceIdentity() {
    if (s_deviceId[0] != '\0') return;
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(s_deviceId, sizeof(s_deviceId), "RM%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    for (size_t i = 0; i < sizeof(s_hostname) && s_deviceId[i]; ++i) {
        const char c = s_deviceId[i];
        s_hostname[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    s_hostname[DEVICE_ID_LEN - 1] = '\0';
}

const char *WiFiSetup::deviceId() {
    initDeviceIdentity();
    return s_deviceId;
}

const char *WiFiSetup::hostname() {
    initDeviceIdentity();
    return s_hostname;
}

// ── Captive Portal ──

void WiFiSetup::startConfigPortalAsync() {
    // Генерация setup-токена (6 hex байт из esp_random)
    for (int i = 0; i < 8; i++) {
        int r = esp_random() % 36;
        s_setupToken[i] = (r < 10) ? ('0' + r) : ('a' + r - 10);
    }
    s_setupToken[8] = '\0';

    // AP+STA обязателен: в чистом WIFI_AP scanNetworks не работает → пустой select.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    String apPass = buildApPassword();
    WiFi.softAP(deviceId(), apPass.c_str());
    Serial.printf("[WIFI] AP '%s' started (password not logged)\n", deviceId());

    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // async; /scan и JS дождутся результата

    // DNS-сервер (перехват всех запросов → 192.168.4.1)
    if (!_dns) {
        _dns = new DNSServer();
    }
    _dns->setErrorReplyCode(DNSReplyCode::NoError);
    _dns->start(53, "*", WiFi.softAPIP());

    // Шаг 4: Веб-сервер на порту 80
    if (!_server) {
        _server = new AsyncWebServer(80);
    }
    setupPortalRoutes();
    _server->begin();

    _configMode = true;
    _portalStartMs = millis();
    Serial.printf("[WIFI] Captive portal: IP=%s SSID=%s\n",
                  WiFi.softAPIP().toString().c_str(), deviceId());
}

void WiFiSetup::stopConfigPortal() {
    _configMode = false;
    if (_server) {
        _server->end();
        delete _server;
        _server = nullptr;
    }
    if (_dns) {
        _dns->stop();
        delete _dns;
        _dns = nullptr;
    }
    WiFi.softAPdisconnect(true);
}

// SoftAP→STA: краткий suppress + обязательный re-arm после stable HIGH.
static uint32_t s_factoryResetSuppressUntilMs = 0;

void WiFiSetup::processDNS() {
    if (_dns) {
        _dns->processNextRequest();
    }
    // /save отвечает 200 сразу; подключение — на следующем тике, пока сервер жив.
    if (s_pendingConnect) {
        s_pendingConnect = false;
        String ssid = s_pendingSsid;
        String pass = s_pendingPass;
        s_pendingSsid = "";
        s_pendingPass = "";
        stopConfigPortal();
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(hostname());
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("[WIFI] Connecting to '%s' after portal save\n", ssid.c_str());
        s_factoryResetSuppressUntilMs =
            millis() + (uint32_t)FACTORY_RESET_WIFI_SUPPRESS_MS;
    }
    pollFactoryResetButton();
}

void WiFiSetup::pollFactoryResetButton() {
    static bool pinReady = false;
    static uint32_t pressStartMs = 0;
    static uint32_t highSinceMs = 0;
    static bool fired = false;
    static bool armed = false;
    if (!pinReady) {
        pinMode(PIN_BTN_FACTORY_RESET, INPUT_PULLUP);
        pinReady = true;
    }

    // XIAO ESP32-S3: USB-Serial/JTAG (HWCDC) при open ACM держит BOOT/GPIO0 LOW
    // (DTR) — ложное срабатывание factory-reset без кнопки. Пока host держит CDC —
    // GPIO0 FR выключен; в поле без Serial кнопка в STA работает; с USB — WebUI reset.
#if ARDUINO_USB_MODE
    if (HWCDC::isConnected()) {
        armed = false;
        highSinceMs = 0;
        pressStartMs = 0;
        fired = false;
        return;
    }
#endif

    if (s_factoryResetSuppressUntilMs != 0 &&
        (int32_t)(millis() - s_factoryResetSuppressUntilMs) < 0) {
        // Важно: disarm. Иначе после suppress sticky-LOW сразу копит hold.
        armed = false;
        highSinceMs = 0;
        pressStartMs = 0;
        fired = false;
        return;
    }

    const bool pressed = (digitalRead(PIN_BTN_FACTORY_RESET) == LOW);
    if (!pressed) {
        if (pressStartMs != 0) {
            Serial.printf("[BTN] release after %ums (no reset)\n",
                          (unsigned)(millis() - pressStartMs));
        }
        pressStartMs = 0;
        fired = false;
        if (highSinceMs == 0) highSinceMs = millis();
        if (!armed &&
            (millis() - highSinceMs) >= (uint32_t)FACTORY_RESET_STABLE_HIGH_MS) {
            armed = true;
        }
        return;
    }

    highSinceMs = 0;
    if (!armed) return;
    if (pressStartMs == 0) {
        pressStartMs = millis();
        Serial.printf("[BTN] press start (need %ums)\n",
                      (unsigned)FACTORY_RESET_HOLD_MS);
        return;
    }
    if (fired) return;
    if ((millis() - pressStartMs) < FACTORY_RESET_HOLD_MS) return;

    fired = true;
    Serial.printf("[WIFI] Factory reset: BOOT held %ums\n",
                  (unsigned)FACTORY_RESET_HOLD_MS);
    resetCredentials();
    delay(200);
    ESP.restart();
}

AsyncWebServer* WiFiSetup::getServer() {
    return _server;
}

// ── NVS ──

bool WiFiSetup::loadCredentials(String &ssid, String &pass) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    ssid = prefs.getString(KEY_SSID, "");
    pass = prefs.getString(KEY_PASS, "");
    prefs.end();
    return (ssid.length() > 0);
}

void WiFiSetup::saveCredentials(const String &ssid, const String &pass) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) {
        Serial.printf("[WIFI] ERROR: NVS begin(%s) failed — credentials NOT saved\n", NVS_NS);
        return;
    }
    size_t nSsid = prefs.putString(KEY_SSID, ssid);
    size_t nPass = prefs.putString(KEY_PASS, pass);
    prefs.end();
    if (nSsid == 0) {
        Serial.printf("[WIFI] ERROR: putString(%s) failed\n", KEY_SSID);
        return;
    }
    Serial.printf("[WIFI] Credentials saved: ssid='%s' (nvs=%u/%u)\n",
                  ssid.c_str(), (unsigned)nSsid, (unsigned)nPass);
}

// ── Подключение к Wi-Fi ──

bool WiFiSetup::tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    configureStaNoSleep();

    // Первая попытка
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (WiFi.status() == WL_CONNECTED) return true;

    // Повторные попытки с переинициализацией (WIFI_RETRY_MAX = 5)
    for (int retry = 0; retry < WIFI_RETRY_MAX; retry++) {
        WiFi.disconnect(true);   // Полный сброс состояния (wipe=true)
        vTaskDelay(pdMS_TO_TICKS(1000));
        WiFi.begin(ssid.c_str(), pass.c_str());
        start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (WiFi.status() == WL_CONNECTED) return true;
    }

    return false;
}

// ── Маршруты Captive Portal ──

void WiFiSetup::setupPortalRoutes() {
    // Главная страница настройки
    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *resp = request->beginResponse(200, "text/html", buildPortalHtml());
        resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(resp);
    });

    // Сохранение настроек и подключение
    _server->on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
        Serial.printf("[WIFI] /save POST received\n");
        if (!request->hasParam("token", true) ||
            request->getParam("token", true)->value() != s_setupToken) {
            Serial.printf("[WIFI] /save REJECTED: invalid setup token\n");
            request->send(403, "text/plain", "Invalid setup token");
            return;
        }
        String ssid = request->hasParam("ssid", true)
            ? request->getParam("ssid", true)->value() : "";
        String pass = request->hasParam("password", true)
            ? request->getParam("password", true)->value() : "";

        if (ssid.length() == 0) {
            Serial.printf("[WIFI] /save REJECTED: empty SSID\n");
            request->send(400, "text/plain", "SSID required");
            return;
        }

        saveCredentials(ssid, pass);

        // Нельзя stopConfigPortal() до send — AsyncWebServer умрёт до ответа.
        s_pendingSsid = ssid;
        s_pendingPass = pass;
        s_pendingConnect = true;
        Serial.printf("[WIFI] /save OK, pending STA connect\n");
        request->send(200, "text/plain", "OK. Connecting...");
    });

    // Сканирование Wi-Fi сетей (JSON)
    _server->on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
        // Scan требует STA; портал держит WIFI_AP_STA.
        if (WiFi.getMode() != WIFI_AP_STA) {
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(deviceId(), buildApPassword().c_str());
        }
        int n = WiFi.scanComplete();
        // -1 / WIFI_SCAN_RUNNING: ещё идёт; -2 / WIFI_SCAN_FAILED: не запущено
        if (n == WIFI_SCAN_RUNNING) {
            request->send(202, "application/json", "[]");
            return;
        }
        if (n == WIFI_SCAN_FAILED || n < 0) {
            WiFi.scanNetworks(true);
            request->send(202, "application/json", "[]");
            return;
        }
        // Формирование JSON массива (до 20 сетей)
        String json = "[";
        for (int i = 0; i < n && i < 20; i++) {
            if (i > 0) json += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            ssid.replace("\n", "");
            ssid.replace("\r", "");
            json += "{\"ssid\":\"" + ssid + "\"";
            json += ",\"rssi\":" + String(WiFi.RSSI(i));
            json += ",\"enc\":\"" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured") + "\"}";
        }
        json += "]";
        WiFi.scanDelete();
        request->send(200, "application/json", json);
    });

    // ── Captive portal detection (совместимость со всеми ОС) ──

    // Android: проверяет http://connectivitycheck.gstatic.com/generate_204
    _server->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    // Apple: проверяет http://captive.apple.com/hotspot-detect.html
    _server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    // Windows NCSI: http://www.msftncsi.com/ncsi.txt
    _server->on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Microsoft NCSI");
    });

    // Windows: http://www.msftconnecttest.com/connecttest.txt
    _server->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "");
    });

    // Microsoft: http://go.microsoft.com/fwlink/?LinkID=219472
    _server->on("/fwlink", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    // Catch-all: любой другой URL → главная страница
    _server->onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });
}

// ── HTML страница портала настройки ──

String WiFiSetup::buildPortalHtml() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RTSPMIC Setup</title>
<style>
:root{--bg:#0a0e17;--panel:#111827;--border:#1f2937;--text:#e5e7eb;--accent:#3b82f6;--muted:#6b7280;}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,sans-serif;background:var(--bg);color:var(--text);display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px;}
.card{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:24px;width:100%;max-width:420px;}
.card h1{font-size:20px;margin-bottom:4px;}
.card p{color:var(--muted);font-size:13px;margin-bottom:20px;}
.form-group{margin-bottom:14px;}
.form-group label{font-size:12px;color:var(--muted);display:block;margin-bottom:4px;}
.form-group select,.form-group input{width:100%;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;color:var(--text);font-size:14px;}
.form-group select option{background:var(--panel);}
.btn{width:100%;background:var(--accent);color:#fff;border:none;border-radius:6px;padding:12px;font-size:14px;font-weight:500;cursor:pointer;}
.btn:hover{opacity:0.9;}
.btn:disabled{opacity:0.5;cursor:not-allowed;}
.status{font-size:12px;margin-top:12px;text-align:center;color:var(--muted);}
.spinner{display:none;width:20px;height:20px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 0.8s linear infinite;margin:12px auto;}
@keyframes spin{to{transform:rotate(360deg);}}
</style>
</head>
<body>
<div class="card">
<h1>RTSPMIC</h1>
<p>Настройка Wi-Fi — __DEVICE_ID__</p>
<form id="wifiForm" onsubmit="return save(event)">
<div class="form-group">
<label for="ssidList">Сеть Wi-Fi</label>
<select id="ssidList">
<option value="">-- поиск --</option>
</select>
</div>
<div class="form-group">
<label for="ssid">Или введите имя сети</label>
<input type="text" id="ssid" name="ssid" required placeholder="Имя сети" autocomplete="off">
</div>
<div class="form-group">
<label for="password">Пароль</label>
<input type="password" id="password" placeholder="Пароль Wi-Fi" autocomplete="off">
</div>
<button type="submit" class="btn" id="saveBtn">Подключить</button>
</form>
<div class="spinner" id="spinner"></div>
<div class="status" id="status"></div>
</div>
<script>
async function scan(attempt){
attempt=attempt||0;
try{
const r=await fetch('/scan');
const nets=await r.json();
const sel=document.getElementById('ssidList');
if(!nets.length){
sel.innerHTML='<option value="">-- поиск'+(attempt>0?' ('+(attempt+1)+')':'')+' --</option>';
if(attempt<15){setTimeout(function(){scan(attempt+1);},1500);}
else{sel.innerHTML='<option value="">-- сети не найдены, введите имя вручную --</option>';document.getElementById('status').textContent='Введите имя сети вручную';}
return;
}
sel.innerHTML='<option value="">-- выберите сеть --</option>';
nets.sort(function(a,b){return b.rssi-a.rssi;}).forEach(function(n){
const o=document.createElement('option');o.value=n.ssid;
o.textContent=n.ssid+(n.enc==='open'?' (открытая)':' ['+n.rssi+' дБм]');
sel.appendChild(o);});
document.getElementById('status').textContent='Найдено сетей: '+nets.length;
}catch(e){
document.getElementById('status').textContent='Ошибка сканирования';
if(attempt<10)setTimeout(function(){scan(attempt+1);},2000);
}}
document.getElementById('ssidList').addEventListener('change',function(){
if(this.value)document.getElementById('ssid').value=this.value;
});
scan();
async function save(e){
e.preventDefault();
const ssid=document.getElementById('ssid').value.trim();
const pass=document.getElementById('password').value;
if(!ssid){document.getElementById('status').textContent='Укажите сеть';return false;}
document.getElementById('saveBtn').disabled=true;
document.getElementById('spinner').style.display='block';
document.getElementById('status').textContent='Подключение...';
try{const r=await fetch('/save',{method:'POST',
headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'token=__SETUP_TOKEN__&ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)});
if(r.ok){document.getElementById('status').textContent='Сохранено, подключаемся...';}
else{document.getElementById('status').textContent='Ошибка '+r.status;document.getElementById('saveBtn').disabled=false;document.getElementById('spinner').style.display='none';}
}catch(err){document.getElementById('status').textContent='Ошибка отправки';document.getElementById('saveBtn').disabled=false;document.getElementById('spinner').style.display='none';}
return false;
}
</script>
</body>
</html>
)rawliteral";
    html.replace("__SETUP_TOKEN__", String(s_setupToken));
    html.replace("__DEVICE_ID__", String(deviceId()));
    return html;
}