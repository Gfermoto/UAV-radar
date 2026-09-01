/**
 * @file    AudioTelemetry.h
 * @brief   Структура телеметрии аудиопотока (без зависимостей от I2S/FreeRTOS).
 *
 * ## Назначение
 *
 * Лёгкая POD-структура (Plain Old Data), используемая AudioProducer
 * для передачи телеметрии потребителям без копирования больших буферов.
 *
 * Обновляется на Core 1 (AudioProducerTask) каждые 100 мс.
 * Читается на Core 0 под защитой _telemetryMutex.
 *
 * ## Поля
 *
 * | Поле            | Тип      | Описание                                  |
 * |-----------------|----------|-------------------------------------------|
 * | peak            | int32_t  | Абсолютный пик за интервал (0..32767)     |
 * | rms             | int32_t  | Среднеквадратичное значение               |
 * | clipping        | bool     | true если было клиппирование              |
 * | overruns        | uint32_t | Счётчик пропусков I2S DMA                 |
 * | samplesRead     | uint32_t | Общее количество прочитанных сэмплов       |
 * | avgDb           | float    | Средний уровень в dBFS                    |
 * | peakDb          | float    | Пиковый уровень в dBFS                    |
 * | confidence      | float    | Уверенность DOA/сигнала (0..1)           |
 * | durationMs      | uint32_t | Длительность непрерывного аудио (мс)      |
 * | lastSampleTimeMs| uint32_t | Timestamp последнего сэмпла для watchdog  |
 * | lastLogTimeMs   | uint32_t | Timestamp последнего логирования ошибок    |
 *
 * @see AudioProducer.h, SystemMonitor.h (liveness), docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef AUDIO_TELEMETRY_H
#define AUDIO_TELEMETRY_H

#include <stdint.h>

struct AudioTelemetry {
    /// @name Уровни сигнала
    /// @{
    int32_t   peak;              ///< Абсолютный пик за интервал (0..32767)
    int32_t   rms;               ///< Среднеквадратичное (root mean square)
    bool      clipping;          ///< true если хотя бы один сэмпл достиг 32767
    float     avgDb;             ///< Средний уровень (dBFS, децибелы full-scale)
    /// @}

    /// @name Счётчики
    /// @{
    uint32_t  overruns;          ///< Пропуски I2S DMA буферов (признак узкого места)
    uint32_t  ringBufferDrops;   ///< Пропуски записи в Ring Buffer (Core 0 не успевает читать)
    uint32_t  samplesRead;       ///< Сэмплов за последний 1-сек интервал (не монотонный!)
    uint32_t  samplesTotal;      ///< Монотонный счётчик сэмплов (liveness kick, wrap-safe)
    /// @}

    /// @name Уровень сигнала / DOA
    /// @{
    float     confidence;        ///< Уверенность DOA/сигнала (0..1)
    uint32_t  durationMs;        ///< Длительность текущего аудио-сегмента (мс)
    /// @}

    /// @name Временные метки
    /// @{
    uint32_t  lastSampleTimeMs;  ///< Время последнего сэмпла (liveness watchdog)
    uint32_t  lastLogTimeMs;     ///< Время последнего логирования ошибок I2S
    /// @}
};

#endif // AUDIO_TELEMETRY_H