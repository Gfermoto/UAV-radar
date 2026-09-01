/**
 * @file    WebUI_Routes.cpp
 * @brief   WebUI: таблица HTTP-маршрутов (SPA, REST API, OTA).
 *
 * Группы эндпоинтов помечены якорями `// ── … ──` со ссылкой на
 * `openapi-webui.yaml`. Auth-уровни — `WebUI_Auth.cpp`.
 * Полный справочник NVS/MQTT: `docs/API_REFERENCE.md`.
 */
#include "WebUI.h"
#include "WebUI_Internal.h"
#include "TcpBudget.h"
#include "TelemetryBuilder.h"
#include "NetConfig.h"
#include "MQTTManager.h"
#include "RTSPClient.h"
#include "RTSPServer.h"
#include "NTPClient.h"
#include "WebCredentials.h"
#include "AudioProducer.h"
#include "XVF3800_I2C.h"
#include "MelSpectrogram.h"
#include <Update.h>
#include <Preferences.h>
#include <cstring>
#include <mbedtls/sha256.h>
#include "Config.h"

void WebUI::setupRoutes() {
    // ── Heap guard: low heap → reject new SPA/WS/TCP (не доводить до OOM-panic).
    const uint32_t freeHeapNow = ESP.getFreeHeap();
    if (freeHeapNow < WEBUI_GUARD_MIN_HEAP) {
        Serial.printf("[WEB] guard: heap=%u < %u — 503 on new conns\n",
                      (unsigned)freeHeapNow, (unsigned)WEBUI_GUARD_MIN_HEAP);
    }

    // ── Main SPA (public dashboard) — openapi: GET / ──
    // Копируем кэш в AsyncResponseStream под _htmlMutex — не держим raw PSRAM
    // указатель в beginResponse_P (UAF при freeHtmlCache/rebuild).
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (ESP.getFreeHeap() < WEBUI_GUARD_MIN_HEAP) {
            request->send(503, "text/plain", "low heap");
            return;
        }
        if (!_htmlMutex ||
            xSemaphoreTake(_htmlMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            request->send(503, "text/plain", "WebUI busy");
            return;
        }
        if (!_htmlCache || _htmlCacheLen == 0) {
            xSemaphoreGive(_htmlMutex);
            request->send(503, "text/plain", "WebUI warming up");
            return;
        }
        const bool wantGzip =
            _htmlGzip && _htmlGzipLen > 0 && request->hasHeader("Accept-Encoding") &&
            request->getHeader("Accept-Encoding")->value().indexOf("gzip") >= 0;
        const size_t bodyLen = wantGzip ? _htmlGzipLen : _htmlCacheLen;
        AsyncResponseStream *resp =
            request->beginResponseStream("text/html", bodyLen + 64);
        if (wantGzip) {
            resp->write(_htmlGzip, _htmlGzipLen);
            resp->addHeader("Content-Encoding", "gzip");
            resp->addHeader("Vary", "Accept-Encoding");
        } else {
            resp->write(reinterpret_cast<const uint8_t *>(_htmlCache), _htmlCacheLen);
        }
        xSemaphoreGive(_htmlMutex);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        request->send(resp);
    });

    // ── PWA manifest (public) ──
    _server->on("/manifest.json", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const char *shortName = "MIC";
        const char *desc = "Сетевой микрофон (RTSP)";
        String json = String("{")
            + "\"name\":\"" FIRMWARE_NAME "\","
            + "\"short_name\":\"" + String(shortName) + "\","
            + "\"description\":\"" + String(desc) + "\","
            + "\"start_url\":\"/\","
            + "\"display\":\"standalone\","
            + "\"background_color\":\"#0a0e17\","
            + "\"theme_color\":\"#0a0e17\","
            + "\"orientation\":\"any\","
            + "\"icons\":[{"
            + "\"src\":\"/manifest.svg\","
            + "\"sizes\":\"any\",\"type\":\"image/svg+xml\""
            + "}]"
            + "}";
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->print(json);
        request->send(resp);
    });

    // ── Manifest SVG icon (public) ──
    _server->on("/manifest.svg", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const char *svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
            "<rect width=\"32\" height=\"32\" rx=\"6\" fill=\"#111827\" stroke=\"#3b82f6\" stroke-width=\"0.5\"/>"
            "<path d=\"M11 14c0-5 4-8 7-8 5 0 6 5 6 8 0 5-3 7-4 9s-1 4-1 4\" stroke=\"#3b82f6\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\"/>"
            "<path d=\"M17 10c-2 0-3 2-3 3\" stroke=\"#3b82f6\" stroke-width=\"2\" fill=\"none\" stroke-linecap=\"round\"/>"
            "</svg>";
        AsyncResponseStream *resp = request->beginResponseStream("image/svg+xml");
        resp->print(svg);
        request->send(resp);
    });

    // ── Service Worker (public, fetch cache for offline resilience) ──
    _server->on("/sw.js", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const char *sw = R"(self.addEventListener('install',e=>self.skipWaiting());
self.addEventListener('activate',e=>e.waitUntil(clients.claim()));
self.addEventListener('fetch',e=>{
  if(e.request.method!='GET')return;
  e.respondWith((async()=>{
    try{return await fetch(e.request);}
    catch{return new Response('',{status:503});}
  })());
});)";
        AsyncResponseStream *resp = request->beginResponseStream("application/javascript");
        resp->print(sw);
        request->send(resp);
    });

    // ── WebSocket ticket — openapi: GET /api/ws-ticket (hardened + rate-limit) ──
    _server->on("/api/ws-ticket", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!requireHardenedAuth(request)) return;
        if (!requireRateOk(request, 500)) return;
        char ticket[WS_TICKET_LEN];
        uint32_t ip = (uint32_t)request->client()->remoteIP();
        issueWsTicket(ip, ticket, sizeof(ticket));
        if (!ticket[0]) {
            request->send(503, "application/json", "{\"error\":\"ticket_issue_failed\"}");
            return;
        }
        String body = String("{\"ticket\":\"") + ticket + "\"}";
        request->send(200, "application/json", body);
    });

    // ── CSRF token — openapi: GET /api/csrf (auth + rate-limit) ──
    _server->on("/api/csrf", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!requireAuth(request)) return;
        if (!requireRateOk(request, 250)) return;
        char tok[CSRF_TOKEN_LEN];
        if (_wsMutex && xSemaphoreTake(_wsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(tok, _csrfToken, sizeof(tok) - 1);
            tok[sizeof(tok) - 1] = '\0';
            xSemaphoreGive(_wsMutex);
        } else {
            strncpy(tok, _csrfToken, sizeof(tok) - 1);
            tok[sizeof(tok) - 1] = '\0';
        }
        String body = String("{\"token\":\"") + tok + "\"}";
        request->send(200, "application/json", body);
    });

    // ── Status — openapi: GET /status (Basic; урезанный payload при default pw) ──
    _server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (ESP.getFreeHeap() < WEBUI_GUARD_MIN_HEAP) {
            request->send(503, "text/plain", "low heap");
            return;
        }
        if (!requireAuth(request)) return;
        const bool fullOk = !WebCredentials::isDefaultPassword();
        // Тот же кэш, что и WebSocket — не строить JSON заново на каждый HTTP poll.
        if (_telemMutex && xSemaphoreTake(_telemMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (fullOk) {
                const uint32_t gen = TelemetryBuilder::sensorGen();
                if (_cachedTelemetryGen != gen) {
                    size_t jsLen = 0;
                    char *ps = TelemetryBuilder::buildAlloc(true, &jsLen, webUiAlloc);
                    if (ps && jsLen > 0 && jsLen <= UINT16_MAX) {
                        if (_cachedTelemetry) webUiFree(_cachedTelemetry);
                        _cachedTelemetry = ps;
                        _cachedTelemetryLen = static_cast<uint16_t>(jsLen);
                        _cachedTelemetryGen = gen;
                    } else if (ps) {
                        webUiFree(ps);
                    }
                }
                if (_cachedTelemetry && _cachedTelemetryLen) {
                    AsyncResponseStream *resp =
                        request->beginResponseStream("application/json");
                    resp->write(reinterpret_cast<const uint8_t *>(_cachedTelemetry),
                                _cachedTelemetryLen);
                    request->send(resp);
                    xSemaphoreGive(_telemMutex);
                    return;
                }
            }
            xSemaphoreGive(_telemMutex);
        }
        size_t len = 0;
        char *buf = fullOk ? TelemetryBuilder::buildAlloc(true, &len, nullptr)
                           : TelemetryBuilder::buildLocatorAlloc(&len, nullptr);
        if (!buf || !len) {
            if (buf) free(buf);
            request->send(503, "application/json", "{\"error\":\"oom\"}");
            return;
        }
        AsyncResponseStream *resp = request->beginResponseStream("application/json");
        resp->write(reinterpret_cast<const uint8_t *>(buf), len);
        request->send(resp);
        free(buf);
    });

    // ── Audio — openapi: POST /api/audio (mutable: auth + CSRF) ──
    _server->on("/api/audio", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireMutable(request)) return;
        NetConfigData cfg;
        NetConfig::load(cfg);
        bool changed = false;
        if (request->hasParam("hpf_mode", true)) {
            int m = request->getParam("hpf_mode", true)->value().toInt();
            if (m >= 0 && m <= 4) {
                cfg.hpfMode = (uint8_t)m;
                changed = true;
            }
        } else if (request->hasParam("hpf", true)) {
            // Legacy: 0/1 → Off / on125; или сразу 0..4 если пришло как mode.
            int h = request->getParam("hpf", true)->value().toInt();
            if (h >= 0 && h <= 4) {
                cfg.hpfMode = (uint8_t)h;
                changed = true;
            }
        }
        if (request->hasParam("hpf_cutoff", true)) {
            // Cutoff derived from mode — ignore free-form Hz.
            (void)request->getParam("hpf_cutoff", true);
        }
        NetConfig::syncHpfFields(cfg);
        if (request->hasParam("agc", true)) {
            bool agc = request->getParam("agc", true)->value() == "1";
            cfg.dspAgcEnabled = agc;
            changed = true;
        }
        if (request->hasParam("cal_offset", true)) {
            float cal = request->getParam("cal_offset", true)->value().toFloat();
            if (cal >= -40.0f && cal <= 40.0f) {
                cfg.calibrationOffsetDb = cal;
                changed = true;
            }
        }
        if (request->hasParam("aec_env", true)) {
            int env = request->getParam("aec_env", true)->value().toInt();
            if (env == 0 || env == 1) {
                cfg.aecEnvMode = (uint8_t)env;
                changed = true;
            }
        }
        if (request->hasParam("dsp_mic_gain", true)) {
            float g = request->getParam("dsp_mic_gain", true)->value().toFloat();
            if (g >= 0.1f && g <= 1000.0f) {
                cfg.dspMicGain = g;
                changed = true;
            }
        }
        if (request->hasParam("loudspeaker_present", true)) {
            cfg.loudspeakerPresent =
                request->getParam("loudspeaker_present", true)->value() == "1";
            changed = true;
        }
        if (request->hasParam("asrout", true)) {
            int v = request->getParam("asrout", true)->value().toInt();
            if (v == 0 || v == 1) {
                cfg.asroutEnabled = (uint8_t)v;
                changed = true;
            }
        }
        if (request->hasParam("echo_suppression", true)) {
            cfg.echoSuppressionEnabled =
                request->getParam("echo_suppression", true)->value() == "1";
            changed = true;
        }
        if (changed) {
            NetConfig::sanitizeLoudspeakerOff(cfg);
            if (!NetConfig::save(cfg)) {
                request->send(500, "application/json", "{\"status\":\"nvs_error\"}");
                return;
            }
            if (_commandCallback) {
                StaticJsonDocument<16> dspDoc;
                _commandCallback("apply_dsp", dspDoc.as<JsonVariant>());
            } else if (_audio) {
                _audio->setHpfMode(cfg.hpfMode);
            }
        }
        TelemetryBuilder::setDspAgcState(cfg.dspAgcEnabled);
        TelemetryBuilder::setDspLimiterState(cfg.dspLimiterEnabled);
        TelemetryBuilder::setAecEnvState(cfg.aecEnvMode);
        TelemetryBuilder::setEchoSuppressionState(cfg.echoSuppressionEnabled);
        TelemetryBuilder::setAsroutState(cfg.asroutEnabled);
        TelemetryBuilder::setLoudspeakerPresentState(cfg.loudspeakerPresent);
        TelemetryBuilder::setDspMicGainState(cfg.dspMicGain);
        TelemetryBuilder::setCalibrationOffsetDb(cfg.calibrationOffsetDb);
        TelemetryBuilder::setDspPathGains(
            cfg.dspMicGain, cfg.asroutGain, cfg.asroutEnabled,
            cfg.attnsMode, cfg.attnsNominal, cfg.attnsSlope);
        DynamicJsonDocument resp(128);
        resp["status"] = "ok";
        resp["calibration_offset_db"] = cfg.calibrationOffsetDb;
        resp["hpf_mode"] = cfg.hpfMode;
        uint8_t hpfRb = 0xFF;
        if (_audio && _audio->getXvf() &&
            _audio->getXvf()->readXvfHpfMode(hpfRb) == XVF3800_Result::OK) {
            resp["hpf_readback"] = hpfRb;
        }
        String out;
        serializeJson(resp, out);
        request->send(200, "application/json", out);
    });

    // ── MEL preview — openapi: GET /api/mel (hardened + rate-limit) ──
    _server->on("/api/mel", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (ESP.getFreeHeap() < WEBUI_GUARD_MIN_HEAP) {
            request->send(503, "text/plain", "low heap");
            return;
        }
        if (!requireHardenedAuth(request)) return;
        if (!requireRateOk(request, 500)) return;
        if (!_audio) { request->send(503, "text/plain", "no audio"); return; }
        const MelSpectrogram *mel = _audio->getMelSpectrogram();
        if (!mel || !mel->isReady()) { request->send(204); return; }
        const int bands = MelSpectrogram::kNumBands;
        const int count = mel->getFrameCount();
        const int nShow = count < 64 ? count : 64;
        if (nShow <= 0) { request->send(204); return; }
        float *spec = (float *)webUiAlloc((size_t)nShow * bands * sizeof(float));
        uint8_t *out = (uint8_t *)webUiAlloc((size_t)nShow * bands);
        if (!spec || !out || !mel->copyRecentFrames(spec, nShow)) {
            if (spec) webUiFree(spec);
            if (out) webUiFree(out);
            request->send(500, "text/plain", "oom");
            return;
        }
        // Per-window min-max: фиксированный (raw+3)*36
        // с mic_gain/AGC даёт mel≈+9 → 50%+ пикселей = 255 («волна» в тишине).
        {
            const size_t n = (size_t)nShow * (size_t)bands;
            float mn = spec[0], mx = spec[0];
            for (size_t i = 1; i < n; ++i) {
                if (spec[i] < mn) mn = spec[i];
                if (spec[i] > mx) mx = spec[i];
            }
            const float span = (mx - mn) < 1e-6f ? 1.0f : (mx - mn);
            for (size_t i = 0; i < n; ++i) {
                int v = (int)((spec[i] - mn) / span * 255.0f + 0.5f);
                out[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        AsyncResponseStream *resp = request->beginResponseStream("application/octet-stream");
        resp->addHeader("X-Mel-Bands", String(bands));
        resp->addHeader("X-Mel-Frames", String(nShow));
        resp->addHeader("X-Mel-Fmin", String(MEL_FMIN));
        resp->addHeader("X-Mel-Fmax", String(MEL_FMAX));
        resp->addHeader("Access-Control-Expose-Headers",
                        "X-Mel-Bands, X-Mel-Frames, X-Mel-Fmin, X-Mel-Fmax");
        resp->write(out, (size_t)nShow * bands);
        webUiFree(spec); webUiFree(out);
        request->send(resp);
    });

    // ── Integrations GET — openapi: GET /api/integrations (hardened) ──
    _server->on("/api/integrations", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!requireHardenedAuth(request)) return;
        if (!requireRateOk(request, 250)) return;
        NetConfigData cfg;
        NetConfig::load(cfg);
        DynamicJsonDocument doc(1280);
        JsonObject obj = doc.to<JsonObject>();
        NetConfig::toJson(obj, cfg, true);
        obj["mqtt_connected"] = _mqtt ? _mqtt->isConnected() : false;
        obj["rtsp_streaming"] = _rtspServer
            ? (_rtspServer->getActiveClientCount() > 0)
            : false;
        obj["ntp_synced"] = _ntp ? _ntp->isSynced() : false;
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ── Integrations POST — openapi: POST /api/integrations (mutable) ──
    _server->on("/api/integrations", HTTP_POST,
        [this](AsyncWebServerRequest *request) {},
        nullptr,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (!requireMutable(request)) return;
            // Чанкованный body: отклоняем по total заранее (иначе RAM/CPU DoS)
            if ((index == 0 && total > 2048) || len > 2048) {
                request->send(413, "application/json", "{\"status\":\"body_too_large\"}");
                return;
            }
            if (index != 0) {
                request->send(413, "application/json", "{\"status\":\"chunked_unsupported\"}");
                return;
            }
            if (!requireRateOk(request, 1000)) return;
            DynamicJsonDocument body(1536);
            if (deserializeJson(body, data, len)) {
                request->send(400, "application/json", "{\"status\":\"bad_json\"}");
                return;
            }
            NetConfigData prev;
            NetConfig::load(prev);
            NetConfigData cfg = prev;
            NetConfig::fromJson(body.as<JsonVariantConst>(), cfg);

            if (cfg.mqttHost[0] && !validateHostname(cfg.mqttHost, sizeof(cfg.mqttHost))) {
                request->send(400, "application/json", "{\"status\":\"bad_mqtt_host\"}");
                return;
            }
            if (cfg.mqttPort == 0) {
                request->send(400, "application/json", "{\"status\":\"bad_port\"}");
                return;
            }
            const bool mqttChanged =
                prev.mqttEnabled != cfg.mqttEnabled ||
                prev.mqttPort != cfg.mqttPort ||
                prev.mqttHaDiscovery != cfg.mqttHaDiscovery ||
                strcmp(prev.mqttHost, cfg.mqttHost) != 0 ||
                strcmp(prev.mqttUser, cfg.mqttUser) != 0 ||
                strcmp(prev.mqttPass, cfg.mqttPass) != 0;
            const bool ntpChanged = strcmp(prev.ntpHost, cfg.ntpHost) != 0;
            SoftApplyResult apply = NetConfig::softApply(
                cfg, mqttChanged, false, ntpChanged,
                false, false, false);
            if (!apply.success) {
                request->send(500, "application/json", "{\"status\":\"nvs_error\"}");
                return;
            }
            if (_mqtt) {
                if (cfg.mqttEnabled) {
                    _mqtt->setEnabled(true);
                    _mqtt->applyBrokerSettings(cfg.mqttHost, cfg.mqttPort,
                                               cfg.mqttUser, cfg.mqttPass, cfg.mqttHaDiscovery);
                    if (!_mqtt->isActive()) _mqtt->begin();
                } else {
                    _mqtt->setEnabled(false);
                    _mqtt->stop();
                }
            }
            TelemetryBuilder::setAudioSetupMode(cfg.audioSetupMode);
            if (_commandCallback) {
                StaticJsonDocument<16> dspDoc;
                _commandCallback("apply_dsp", dspDoc.as<JsonVariant>());
                StaticJsonDocument<16> sysDoc;
                _commandCallback("apply_system", sysDoc.as<JsonVariant>());
            }
            if (_ntp && cfg.ntpHost[0]) {
                _ntp->setServer(cfg.ntpHost);
            }
            DynamicJsonDocument resp(128);
            resp["status"] = "ok";
            resp["reboot_recommended"] = apply.rebootRecommended;
            String out;
            serializeJson(resp, out);
            request->send(200, "application/json", out);
        });

    // ── System — openapi: POST /api/system/reboot ──
    _server->on("/api/system/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireMutable(request)) return;
        request->send(200, "text/plain", "Rebooting...");
        scheduleRestart();
    });

    // ── System — openapi: POST /api/system/factory_reset ──
    _server->on("/api/system/factory_reset", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireMutable(request)) return;
        {
            Preferences prefs;
            prefs.begin("rtspmic", false);
            prefs.clear();
            prefs.end();
        }
        request->send(200, "text/plain", "Reset. Rebooting...");
        scheduleRestart();
    });

    // ── System — openapi: POST /api/system/password (auth + CSRF; default pw OK) ──
    _server->on("/api/system/password", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireAuth(request) || !requireCsrf(request)) return;
        // Отдельный лимит: общий RATE слот делили /api/csrf (SPA) → вечный 429.
        if (!request->client()) return;
        const uint32_t ip = (uint32_t)request->client()->remoteIP();
        static uint32_t s_lastPwIp = 0;
        static uint32_t s_lastPwMs = 0;
        const uint32_t now = millis();
        if (ip != 0 && ip == s_lastPwIp && (now - s_lastPwMs) < 1000u) {
            request->send(429, "application/json", "{\"status\":\"rate_limited\"}");
            return;
        }
        s_lastPwIp = ip;
        s_lastPwMs = now;
        String user = request->hasParam("user", true)
            ? request->getParam("user", true)->value() : "";
        String pass = request->hasParam("password", true)
            ? request->getParam("password", true)->value() : "";
        char curUser[WEB_CRED_USER_MAX + 1];
        char curPass[WEB_CRED_PASS_MAX];
        WebCredentials::load(curUser, sizeof(curUser), curPass, sizeof(curPass));
        if (user.length() == 0) user = curUser;
        if (!WebCredentials::validateUser(user.c_str())) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"msg\":\"bad_user\"}");
            return;
        }
        if (!WebCredentials::validatePass(pass.c_str())) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"msg\":\"password min 8 chars\"}");
            return;
        }
        if (WebCredentials::ctEq(pass.c_str(), WEB_UI_DEFAULT_PASSWORD)) {
            request->send(400, "application/json",
                          "{\"status\":\"error\",\"msg\":\"must_not_be_default\"}");
            return;
        }
        if (!WebCredentials::save(user.c_str(), pass.c_str())) {
            request->send(500, "application/json", "{\"status\":\"nvs_error\"}");
            return;
        }
        if (_rtspServer) _rtspServer->reloadCredentials();
        refreshCsrf();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    // System LED: mic ring DoA/off + ESP status. See docs/LED.md, openapi /api/system/led.
    // Public: on|off. Aliases: status|0 → on; level|2 (deprecated) → on.
    // ── System LED — openapi: POST /api/system/led (mutable) ──
    _server->on("/api/system/led", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireMutable(request)) return;
        String mode = request->hasParam("mode", true)
            ? request->getParam("mode", true)->value() : "";
        mode.trim();
        mode.toLowerCase();
        const char *canon = nullptr;
        if (mode == "on" || mode == "status" || mode == "0" ||
            mode == "level" || mode == "2") {
            canon = "status";
        } else if (mode == "off" || mode == "1") {
            canon = "off";
        }
        if (canon) {
            StaticJsonDocument<64> doc;
            doc["v"] = canon;
            if (_commandCallback) _commandCallback("led_mode", doc["v"]);
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"status\":\"bad_mode\"}");
        }
    });

    // ── Network — openapi: GET /api/network/scan (hardened + rate-limit) ──
    _server->on("/api/network/scan", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!requireHardenedAuth(request)) return;
        if (!requireRateOk(request, 1000)) return;
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING || n < 0) {
            WiFi.scanNetworks(true);
            request->send(202, "application/json", "[]");
            return;
        }
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject entry = arr.createNestedObject();
            entry["ssid"] = WiFi.SSID(i);
            entry["rssi"] = WiFi.RSSI(i);
            entry["enc"]  = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured";
        }
        String json;
        serializeJson(doc, json);
        WiFi.scanDelete();
        request->send(200, "application/json", json);
    });

    // ── Network — openapi: POST /api/network/config (mutable) ──
    _server->on("/api/network/config", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!requireMutable(request)) return;
        String ssid = request->hasParam("ssid", true)
            ? request->getParam("ssid", true)->value() : "";
        String pass = request->hasParam("password", true)
            ? request->getParam("password", true)->value() : "";
        if (ssid.length() > 0) {
            Preferences prefs;
            prefs.begin("rtspmic", false);
            prefs.putString("wifi_ssid", ssid);
            prefs.putString("wifi_pass", pass);
            prefs.end();
        }
        request->send(200, "text/plain", "Saved. Rebooting...");
        scheduleRestart();
    });

    // ── OTA — openapi: GET /update (auth) ──
    _server->on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!requireAuth(request)) return;
        request->send(200, "text/plain",
                      "Use WebUI OTA tab (authenticated SPA with CSRF).");
    });

    // ── OTA — openapi: POST /update (mutable, multipart .bin) ──
    _server->on("/update", HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (!requireMutable(request)) return;
            if (_otaState.phase != OtaPhase::PENDING_VERIFY || !_otaState.hashValid) {
                if (_otaState.phase == OtaPhase::SUCCESS) {
                    request->send(200, "text/plain", "OK. Rebooting...");
                    scheduleRestart();
                    return;
                }
                request->send(500, "text/plain", "OTA incomplete");
                return;
            }
            NetConfigData cfg;
            NetConfig::load(cfg);
            if (cfg.hmacKey.valid) {
                if (!request->hasHeader("X-Firmware-Signature")) {
                    Update.abort();
                    _otaState.phase = OtaPhase::ABORTED;
                    request->send(403, "text/plain", "Firmware signature required");
                    return;
                }
                uint8_t mac[32];
                const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
                if (!info ||
                    mbedtls_md_hmac(info, cfg.hmacKey.bytes, sizeof(cfg.hmacKey.bytes),
                                    _otaState.sha256, sizeof(_otaState.sha256), mac) != 0) {
                    Update.abort();
                    _otaState.phase = OtaPhase::ABORTED;
                    request->send(500, "text/plain", "Signature check failed");
                    return;
                }
                char expected[65];
                for (int i = 0; i < 32; i++) snprintf(expected + i * 2, 3, "%02x", mac[i]);
                const String &sig = request->getHeader("X-Firmware-Signature")->value();
                if (!sig.equalsIgnoreCase(expected)) {
                    Update.abort();
                    _otaState.phase = OtaPhase::ABORTED;
                    request->send(403, "text/plain", "Invalid firmware signature");
                    return;
                }
            }
            if (!Update.end(true)) {
                Update.printError(Serial);
                _otaState.phase = OtaPhase::ABORTED;
                request->send(500, "text/plain", "OTA finalize failed");
                return;
            }
            _otaState.phase = OtaPhase::SUCCESS;
            request->send(200, "text/plain", "OK. Rebooting...");
            scheduleRestart();
        },
        [this](AsyncWebServerRequest *request, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            if (!requireMutable(request)) {
                if (!index) {
                    _otaState.phase = OtaPhase::ABORTED;
                    Update.abort();
                }
                return;
            }
            if (!index) {
                if (!filename.endsWith(".bin")) {
                    Serial.printf("[OTA] Rejected non-.bin: %s\n", filename.c_str());
                    _otaState.phase = OtaPhase::ABORTED;
                    Update.abort();
                    return;
                }
                _otaState.phase = OtaPhase::UPLOADING;
                _otaState.totalBytes = 0;
                _otaState.hashValid = false;
                _otaStartMs = millis();
                mbedtls_sha256_init(&_otaShaCtx);
                mbedtls_sha256_starts(&_otaShaCtx, 0);
                _otaHashing = true;
                Serial.printf("[OTA] Start: %s (%u B)\n", filename.c_str(),
                              (unsigned)request->contentLength());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    Update.printError(Serial);
                    _otaState.phase = OtaPhase::ABORTED;
                    Update.abort();
                    return;
                }
            }
            if (_otaState.phase == OtaPhase::ABORTED) {
                if (_otaHashing) { mbedtls_sha256_free(&_otaShaCtx); _otaHashing = false; }
                return;
            }
            if (_otaHashing) mbedtls_sha256_update(&_otaShaCtx, data, len);
            _otaState.totalBytes += len;
            if (Update.write(data, len) != len) {
                Update.printError(Serial);
                _otaState.phase = OtaPhase::ABORTED;
                Update.abort();
                return;
            }
            if (final) {
                uint32_t elapsed = millis() - _otaStartMs;
                if (_otaHashing) {
                    mbedtls_sha256_finish(&_otaShaCtx, _otaState.sha256);
                    mbedtls_sha256_free(&_otaShaCtx);
                    _otaHashing = false;
                    _otaState.hashValid = true;
                }
                _otaState.phase = OtaPhase::PENDING_VERIFY;
                Serial.printf("[OTA] Uploaded %u B, awaiting verify\n",
                              (unsigned)_otaState.totalBytes);
            }
        });

    // ── 404 ──
    _server->onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not Found");
    });
}
