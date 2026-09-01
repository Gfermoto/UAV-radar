/**
 * @file    AudioConvert.h
 * @brief   Конвертация I2S 32-бит стерео → 16-бит моно PCM (тестируемая логика).
 *
 * XVF3800: L = processed auto-select beam (OP_L 6,3), R = silence.
 * Берём только Left — иначе (L+R)/2 даёт −6 dB.
 *
 * gainQ16 — digital gain в формате Q16 (1.0 = 65536). Выход: rmsAcc, peak за блок.
 * Header-only; unit-тесты на host без I2S.
 *
 * @see AudioProducer.h, docs/ARCHITECTURE.md
 */

#ifndef AUDIO_CONVERT_H
#define AUDIO_CONVERT_H

#include <stdint.h>
#include <stddef.h>

#ifndef PCM_MAX_AMP
#define PCM_MAX_AMP 32767
#endif
#ifndef PCM_MIN_AMP
#define PCM_MIN_AMP -32768
#endif

inline size_t audioConvertI2SToMono(const int32_t *i2sBuf, size_t samples,
                                    int16_t *pcmBuf, int32_t gainQ16,
                                    int64_t *outRmsAcc, int32_t *outPeak) {
    if (!i2sBuf || !pcmBuf || samples < 2) {
        return 0;
    }

    size_t monoCount = 0;
    size_t pairs = samples / 2;
    int64_t rmsAcc = 0;
    int32_t peak = 0;

    for (size_t i = 0; i < pairs && i < 4; i++) {
        // Диагностика: смотрим все 32 бита первого канала
        int32_t ch0  = i2sBuf[i * 2];
        int32_t ch1  = i2sBuf[i * 2 + 1];
        // Сохраняем raw через telemetry
        int32_t hi16 = ch0 >> 16;
        int32_t lo16 = ch0 & 0xFFFF;
    }
    for (size_t i = 0; i < pairs; i++) {
        int32_t ch0  = i2sBuf[i * 2];
        (void)i2sBuf[i * 2 + 1];

        int32_t sample = ch0 >> 16;  // upper 16 bits
        if (sample & 0x8000) sample |= ~0xFFFF;
        int16_t left16 = static_cast<int16_t>(sample);
        int32_t mono = static_cast<int32_t>(left16);

        int32_t amplified = static_cast<int32_t>((static_cast<int64_t>(mono) * gainQ16) >> 16);
        if (amplified > PCM_MAX_AMP)  amplified = PCM_MAX_AMP;
        if (amplified < PCM_MIN_AMP) amplified = PCM_MIN_AMP;

        int32_t absVal = amplified < 0 ? -amplified : amplified;
        if (absVal > peak) peak = absVal;
        rmsAcc += static_cast<int64_t>(amplified) * amplified;

        pcmBuf[monoCount++] = static_cast<int16_t>(amplified);
    }

    if (outRmsAcc) *outRmsAcc = rmsAcc;
    if (outPeak)   *outPeak = peak;
    return monoCount;
}

#endif // AUDIO_CONVERT_H
