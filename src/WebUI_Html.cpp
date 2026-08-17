/**
 * @file    WebUI_Html.cpp
 * @brief   WebUI: сборка SPA, i18n replace, gzip-кэш в PSRAM.
 *
 * Шаблон — `WEBUI_HTML_PAGE` из `WebUI_page.h`. Отдача через `GET /`
 * (plain или gzip). Не вызывать сборку из Async callback — только webTask.
 */
#include "WebUI.h"
#include "WebUI_Internal.h"
#include "WebUI_i18n.h"
#include "WebUI_page.h"
#include "WebCredentials.h"
#include "Config.h"
#include <cstring>
extern "C" {
#include "esp32s3/rom/miniz.h"
}

void WebUI::freeHtmlCache() {
    auto freeBoth = [this]() {
        if (_htmlCache) {
            webUiFree(_htmlCache);
            _htmlCache = nullptr;
            _htmlCacheLen = 0;
        }
        if (_htmlGzip) {
            webUiFree(_htmlGzip);
            _htmlGzip = nullptr;
            _htmlGzipLen = 0;
        }
    };
    if (_htmlMutex) {
        if (xSemaphoreTake(_htmlMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            freeBoth();
            xSemaphoreGive(_htmlMutex);
        }
        // Без lock не free — GET / мог ещё копировать кэш в response stream.
        return;
    }
    freeBoth();
}

bool WebUI::ensureHtmlCache() {
    if (_htmlMutex &&
        xSemaphoreTake(_htmlMutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        Serial.printf("[WEB] HTML cache lock timeout\n");
        return false;
    }

    if (_htmlCache && _htmlCacheLen > 0 &&
        strstr(_htmlCache, "</html>") && strstr(_htmlCache, "<script") &&
        _htmlGzip && _htmlGzipLen > 0) {
        if (_htmlMutex) xSemaphoreGive(_htmlMutex);
        return true;
    }
    if (_htmlCache) {
        webUiFree(_htmlCache);
        _htmlCache = nullptr;
        _htmlCacheLen = 0;
    }
    if (_htmlGzip) {
        webUiFree(_htmlGzip);
        _htmlGzip = nullptr;
        _htmlGzipLen = 0;
    }

    const size_t srcLen = strlen(WEBUI_HTML_PAGE);
    Serial.printf("[WEB] HTML flash len=%u heap=%u psram=%u\n",
                  (unsigned)srcLen, ESP.getFreeHeap(), ESP.getFreePsram());

    size_t cap = srcLen + 24576;
    char *buf = static_cast<char *>(webUiAlloc(cap));
    if (!buf) {
        Serial.printf("[WEB] HTML PSRAM alloc %u failed\n", (unsigned)cap);
        if (_htmlMutex) xSemaphoreGive(_htmlMutex);
        return false;
    }
    memcpy(buf, WEBUI_HTML_PAGE, srcLen + 1);
    size_t len = srcLen;

    auto rep = [&](const char *from, const char *to) -> bool {
        if (!webUiReplaceAll(&buf, &len, &cap, from, to)) {
            Serial.printf("[WEB] HTML replace failed: %s\n", from);
            return false;
        }
        return true;
    };

    bool ok = true;
    ok = ok && rep("RTSPMIC_PLACEHOLDER_NAME", FIRMWARE_NAME);
    ok = ok && rep("RTSPMIC_BODY_PROFILE", "profile-dev");
    char portBuf[8];
    snprintf(portBuf, sizeof(portBuf), "%u", (unsigned)WEB_PORT);
    ok = ok && rep("RTSPMIC_PLACEHOLDER_WEBPORT", portBuf);
    ok = ok && rep("RTSPMIC_PLACEHOLDER_VERSION", FIRMWARE_VERSION);
    ok = ok && rep("RTSPMIC_CSRF_TOKEN_JS", "var CSRF_TOKEN='';");
    ok = ok && rep("'RTSPMIC_WEB_USER'", "''");
    ok = ok && rep("\"RTSPMIC_WEB_USER\"", "\"\"");
    char rateBuf[12];
    snprintf(rateBuf, sizeof(rateBuf), "%u", (unsigned)I2S_SAMPLE_RATE);
    ok = ok && rep("RTSPMIC_SAMPLE_RATE", rateBuf);
    ok = ok && rep("RTSPMIC_ETH_NET_ROW",
        "<div class=\"stat-row\"><span class=\"stat-label\">__T_NET_ETHERNET__</span>"
        "<span id=\"netEth\" class=\"stat-value\">—</span></div>");
    ok = ok && rep("RTSPMIC_DEFAULT_PW_JS",
                   "var RTSPMIC_DEFAULT_PW=0; /* set from /status after Basic Auth */");
    ok = ok && rep("__HTML_LANG__", WEBUI_HTML_LANG);

    if (ok) {
        const size_t n = sizeof(kWebUiI18n) / sizeof(kWebUiI18n[0]);
        for (size_t i = 0; i < n && ok; ++i) {
            char ph[48];
            snprintf(ph, sizeof(ph), "__T_%s__", kWebUiI18n[i].key);
#if defined(WEBUI_LANG_EN)
            ok = rep(ph, kWebUiI18n[i].en);
#else
            ok = rep(ph, kWebUiI18n[i].ru);
#endif
            if ((i & 7) == 7) delay(0);  // WDT / yield на длинном i18n
        }
    }

    if (!ok || !strstr(buf, "</html>") || !strstr(buf, "<script")) {
        Serial.printf("[WEB] HTML build incomplete ok=%d len=%u\n", (int)ok, (unsigned)len);
        webUiFree(buf);
        if (_htmlMutex) xSemaphoreGive(_htmlMutex);
        return false;
    }

    _htmlCache = buf;
    _htmlCacheLen = len;

    // gzip once: tdefl_compressor ~100KB — только в PSRAM (стек webTask 12KB).
    {
        struct GzSink {
            uint8_t *buf{nullptr};
            size_t len{0};
            size_t cap{0};
        };
        auto put = [](const void *pBuf, int n, void *user) -> mz_bool {
            auto *s = static_cast<GzSink *>(user);
            if (n <= 0) return MZ_TRUE;
            if (s->len + static_cast<size_t>(n) > s->cap) return MZ_FALSE;
            memcpy(s->buf + s->len, pBuf, static_cast<size_t>(n));
            s->len += static_cast<size_t>(n);
            return MZ_TRUE;
        };

        auto *comp = static_cast<tdefl_compressor *>(
            webUiAlloc(sizeof(tdefl_compressor)));
        // HTML хорошо жмётся; запас на редкое расширение deflate.
        const size_t outCap = 10u + len + 512u + 8u;
        uint8_t *out = static_cast<uint8_t *>(webUiAlloc(outCap));
        if (!comp || !out) {
            Serial.printf("[WEB] HTML gzip alloc failed — plain only\n");
            webUiFree(comp);
            webUiFree(out);
        } else {
            static const uint8_t kHdr[10] = {
                0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff};
            memcpy(out, kHdr, sizeof(kHdr));
            GzSink sink;
            sink.buf = out + 10;
            sink.len = 0;
            sink.cap = outCap - 18u;
            // Raw deflate (no zlib header) inside gzip container.
            tdefl_init(comp, put, &sink,
                       TDEFL_DEFAULT_MAX_PROBES | TDEFL_GREEDY_PARSING_FLAG);
            const tdefl_status st =
                tdefl_compress_buffer(comp, buf, len, TDEFL_FINISH);
            if (st == TDEFL_STATUS_DONE && sink.len > 0 &&
                sink.len + 18 <= outCap) {
                const uint32_t crc = static_cast<uint32_t>(
                    mz_crc32(MZ_CRC32_INIT,
                             reinterpret_cast<const unsigned char *>(buf),
                             len));
                const uint32_t isize = static_cast<uint32_t>(len);
                memcpy(out + 10 + sink.len, &crc, 4);
                memcpy(out + 10 + sink.len + 4, &isize, 4);
                _htmlGzip = out;
                _htmlGzipLen = 10 + sink.len + 8;
                out = nullptr;  // ownership → cache
                Serial.printf("[WEB] HTML gzip ready %u → %u B (%.0f%%)\n",
                              (unsigned)len, (unsigned)_htmlGzipLen,
                              100.0f * (float)_htmlGzipLen / (float)len);
            } else {
                Serial.printf("[WEB] HTML gzip compress failed st=%d out=%u\n",
                              (int)st, (unsigned)sink.len);
            }
            webUiFree(comp);
            webUiFree(out);
        }
    }

    Serial.printf("[WEB] HTML cache ready %u B\n", (unsigned)len);
    if (_htmlMutex) xSemaphoreGive(_htmlMutex);
    return true;
}

String WebUI::buildHtmlPage() const {
    String html = String(WEBUI_HTML_PAGE);
    html.replace("RTSPMIC_PLACEHOLDER_NAME", FIRMWARE_NAME);
    html.replace("RTSPMIC_BODY_PROFILE", "profile-dev");
    html.replace("RTSPMIC_PLACEHOLDER_WEBPORT", String(WEB_PORT));
    html.replace("RTSPMIC_PLACEHOLDER_VERSION", FIRMWARE_VERSION);
    // CSRF only via GET /api/csrf after Basic — never embed in public HTML.
    html.replace("RTSPMIC_CSRF_TOKEN_JS", "var CSRF_TOKEN='';");
    char webPass[WEB_CRED_PASS_MAX];
    char webUser[WEB_CRED_USER_MAX + 1];
    WebCredentials::load(webUser, sizeof(webUser), webPass, sizeof(webPass));
    (void)webUser;
    // Do not leak username in public HTML.
    html.replace("'RTSPMIC_WEB_USER'", "''");
    html.replace("\"RTSPMIC_WEB_USER\"", "\"\"");
    html.replace("RTSPMIC_SAMPLE_RATE", String(I2S_SAMPLE_RATE));
    html.replace("RTSPMIC_ETH_NET_ROW",
        "<div class=\"stat-row\"><span class=\"stat-label\">__T_NET_ETHERNET__</span>"
        "<span id=\"netEth\" class=\"stat-value\">—</span></div>");
    html.replace("RTSPMIC_DEFAULT_PW_JS",
                 "var RTSPMIC_DEFAULT_PW=0; /* set from /status after Basic Auth */");
    webUiApplyI18n(html);
    return html;
}
