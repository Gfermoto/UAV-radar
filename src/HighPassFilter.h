/**
 * @file    HighPassFilter.h
 * @brief   Butterworth HPF 2-го порядка, 120 Гц. Подавление шума ветра.
 *
 * ## Назначение
 *
 * Софт Butterworth HPF — **только если !_xvf** (нет XVF на шине).
 * При подключённом XVF3800 ФВЧ включается аппаратно (AEC_HPFONOFF), софт-путь
 * отключён в AudioProducer::setHpfMode() — без двойной фильтрации.
 *
 * Фильтр отсекает низкочастотный шум ветра (<100 Гц) при сохранении
 * вокализаций птиц и полезного аудиосигнала.
 *
 * Оптимальный cutoff — 120 Гц по результатам сетки 80/100/120 Гц.
 * Слишком высокий cutoff может срезать низкие тона.
 *
 * ## Реализация
 *
 * Direct Form II Transposed Structure — минимальное количество операций
 * и состояний. Коэффициенты вычисляются по стандартной формуле Butterworth
 * через билинейное преобразование (Q = 1/sqrt(2) = 0.7071).
 *
 * Разностное уравнение:
 *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
 *
 * Состояния: (_x1, _x2) — входные, (_y1, _y2) — выходные.
 *
 * ## Производительность
 *
 * Все операции над float, конвертация int16_t ↔ float на каждом сэмпле.
 * ~10 тактов на сэмпл (при 16 кГц — менее 0.01% CPU).
 *
 * @see AudioProducer.h (setHpfMode), docs/ARCHITECTURE.md
 *
 * @author  RTSPMIC Team
 * @date    2026-07-01
 */

#ifndef HIGH_PASS_FILTER_H
#define HIGH_PASS_FILTER_H

#include <Arduino.h>
#include <mutex>
#include "Config.h"

/**
 * @brief Фильтр высоких частот: Butterworth 2-го порядка.
 *
 * ## Использование
 *
 * @code
 *   HighPassFilter hpf(120.0f, 16000.0f);
 *   hpf.setEnabled(true);  // Ветрозащита
 *
 *   // Посэмпловая обработка:
 *   int16_t filtered = hpf.process(rawSample);
 *
 *   // Блочная обработка (быстрее, меньше вызовов):
 *   hpf.processBlock(pcmBuf, 512);
 * @endcode
 *
 * @note Фильтр может быть динамически включён/выключен
 *       и перенастроен на другую частоту среза через setCutoff().
 */
class HighPassFilter {
public:
    struct Snapshot {
        bool enabled;
        float cutoffHz;
    };

    /**
     * @brief Создание фильтра.
     * @param cutoffHz     Частота среза (default: 120 Гц).
     * @param sampleRateHz Частота дискретизации (default: 16000 Гц).
     */
    explicit HighPassFilter(float cutoffHz = 120.0f, float sampleRateHz = 16000.0f);

    /**
     * @brief Обработка одного сэмпла.
     * @param sample 16-бит PCM сэмпл.
     * @return Отфильтрованный сэмпл (16 бит, с насыщением).
     */
    int16_t process(int16_t sample);

    /**
     * @brief Блочная обработка массива PCM-сэмплов in-place.
     * @param buffer Массив сэмплов.
     * @param count  Количество сэмплов.
     */
    void processBlock(int16_t *buffer, size_t count);

    /** @brief Включение/выключение фильтра. */
    void setEnabled(bool enabled);

    /** @return true если фильтр активен. */
    bool isEnabled() const;

    /** @brief Изменение частоты среза (пересчёт коэффициентов). */
    void setCutoff(float cutoffHz);

    /** @return Текущая частота среза (Гц). */
    float getCutoff() const;

    Snapshot getSnapshot() const;

private:
    mutable std::mutex _mutex;
    bool _enabled;
    float _cutoffHz;

    float _b0, _b1, _b2;
    float _a1, _a2;

    float _x1, _x2;
    float _y1, _y2;

    float _sampleRate;

    void calculateCoefficients(float cutoffHz);
    int16_t processUnlocked(int16_t sample);
};

#endif // HIGH_PASS_FILTER_H