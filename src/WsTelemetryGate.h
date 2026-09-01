/**
 * @file    WsTelemetryGate.h
 * @brief   Чистая логика backpressure для push телеметрии по WebSocket (unit-testable).
 *
 * ## Контракт
 *
 * `wsTelemetryDecide()` — единая точка решения «слать / пропустить» перед
 * `AsyncWebSocket::textAll`. Не зависит от Arduino/AsyncWeb — тестируется на host.
 *
 * Порядок проверок (fail-fast):
 *   1. `running` — WebUI активен
 *   2. `freeHeap >= minHeap` — не аллоцировать/слать при OOM
 *   3. `0 < payloadLen <= maxPayload` — защита от пустого/гигантского JSON
 *   4. `authed` — клиент прошёл WS ticket
 *   5. `canSend && !queueFull` — backpressure очереди AsyncTCP
 *
 * Флаги `skip*` в `WsTelemetryDecision` — причина отказа для метрик/логов.
 *
 * @see WebUI::broadcastTelemetry, WS_TELEM_MIN_HEAP, WS_TELEM_MAX_PAYLOAD
 */
#ifndef WS_TELEMETRY_GATE_H
#define WS_TELEMETRY_GATE_H

#include <stddef.h>
#include <stdint.h>

/** Результат решения о push телеметрии. */
struct WsTelemetryDecision {
    bool send = false;
    bool skipUnauthed = false;
    bool skipBackpressure = false;
    bool skipLowHeap = false;
    bool skipOversize = false;
};

/** Минимум свободной кучи перед alloc/send телеметрии. */
static constexpr uint32_t WS_TELEM_MIN_HEAP = 20000;
/** Мягкий лимит размера одного JSON push (байт). */
static constexpr size_t WS_TELEM_MAX_PAYLOAD = 12288;

/**
 * @brief Решение о push телеметрии.
 * @param running     WebUI запущен.
 * @param authed      WS-клиент авторизован (ticket).
 * @param canSend     AsyncWebSocket готов слать.
 * @param queueFull   Очередь исходящих переполнена.
 * @param freeHeap    ESP.getFreeHeap().
 * @param payloadLen  Длина JSON.
 */
inline WsTelemetryDecision wsTelemetryDecide(bool running,
                                            bool authed,
                                            bool canSend,
                                            bool queueFull,
                                            uint32_t freeHeap,
                                            size_t payloadLen,
                                            uint32_t minHeap = WS_TELEM_MIN_HEAP,
                                            size_t maxPayload = WS_TELEM_MAX_PAYLOAD) {
    WsTelemetryDecision d;
    if (!running) {
        return d;
    }
    if (freeHeap < minHeap) {
        d.skipLowHeap = true;
        return d;
    }
    if (payloadLen == 0 || payloadLen > maxPayload) {
        d.skipOversize = true;
        return d;
    }
    if (!authed) {
        d.skipUnauthed = true;
        return d;
    }
    if (!canSend || queueFull) {
        d.skipBackpressure = true;
        return d;
    }
    d.send = true;
    return d;
}

#endif
