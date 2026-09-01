/**
 * @file DspLevelCompensate.h
 * @brief Компенсация DSP-усилений для абсолютных SPL / noise / LAeq.
 *
 * I2S измеряется после цепочки: mic_gain → … → AGC → ATTNS(non-speech) → ASROUT.
 * acoustic_db ≈ raw_db − effective_gain_db [+ калибровка NVS].
 *
 * ATTNS (XMOS UG): доп. множитель в non-speech:
 *   factor = nominal * (G_init / G_current)^slope
 * PP_AGCGAIN readout, скорее всего, без этого множителя — учитываем отдельно.
 *
 * dspEffectiveGainDb() суммирует AGC + ATTNS + mic + asrout + softwareGain.
 * dspCompensateLevelDb(raw, gain) — вычитает суммарное усиление из dBFS/dB.
 *
 * @see SoundLevelMeter.h, XVF3800_Cache.h, docs/ARCHITECTURE.md
 */

#ifndef DSP_LEVEL_COMPENSATE_H
#define DSP_LEVEL_COMPENSATE_H

#include <math.h>
#include <cmath>
#include <stdint.h>

/** XMOS appendix default PP_AGCGAIN (linear) — AGCGAIN_INIT для формулы ATTNS. */
#ifndef DSP_AGC_GAIN_INIT_LIN
#define DSP_AGC_GAIN_INIT_LIN 32.0f
#endif

struct DspLevelCompParams {
    float    agcGainDb;       ///< PP_AGCGAIN → dB
    bool     agcEnabled;      ///< PP_AGCONOFF; ATTNS inert if false
    float    micGainLin;      ///< AUDIO_MGR_MIC_GAIN
    float    asroutGainLin;   ///< AEC_ASROUTGAIN
    bool     asroutEnabled;   ///< ASROUT beam path (gain applies)
    uint8_t  attnsMode;       ///< PP_ATTNS_MODE
    float    attnsNominal;    ///< PP_ATTNS_NOMINAL
    float    attnsSlope;      ///< PP_ATTNS_SLOPE
    bool     speechActive;    ///< ATTNS only in non-speech
    float    agcGainInitLin;  ///< AGCGAIN_INIT (default 32)
    float    softwareGainLin; ///< AudioProducer digital gain after I2S (1.0 = unity)
};

inline float dspLinToDb(float lin) {
    if (!(lin > 1e-6f) || !std::isfinite(lin)) return -120.0f;
    return 20.0f * log10f(lin);
}

inline float dspDbToLin(float db) {
    if (!std::isfinite(db)) return 1.0f;
    return powf(10.0f, db / 20.0f);
}

/** Множитель ATTNS на выходе (1 = нет доп. ослабления). */
inline float dspAttnsFactor(const DspLevelCompParams &p) {
    if (!p.attnsMode || !p.agcEnabled || p.speechActive) return 1.0f;
    float nom = p.attnsNominal;
    if (!(nom >= 0.0f) || !std::isfinite(nom)) nom = 1.0f;
    if (nom > 1.0f) nom = 1.0f;

    float slope = p.attnsSlope;
    if (!(slope >= 0.0f) || !std::isfinite(slope)) slope = 0.0f;
    if (slope > 5.0f) slope = 5.0f;

    float gInit = p.agcGainInitLin > 1e-6f ? p.agcGainInitLin : DSP_AGC_GAIN_INIT_LIN;
    float gCur = dspDbToLin(p.agcGainDb);
    if (gCur < 1e-6f) gCur = 1e-6f;

    float ratio = gInit / gCur;
    float factor = nom * powf(ratio, slope);
    if (!(factor > 1e-6f) || !std::isfinite(factor)) factor = 1e-6f;
    if (factor > 1000.0f) factor = 1000.0f;
    return factor;
}

/**
 * Суммарное усиление на пути измерения (dB).
 * effective = AGC + ATTNS + mic + (asrout if beam) + softwareGain
 */
inline float dspEffectiveGainDb(const DspLevelCompParams &p) {
    // AGC off: PP_AGCGAIN не входит в акустический уровень (фикс. путь ≈ 0 dB AGC).
    float total = 0.0f;
    if (p.agcEnabled) {
        total = p.agcGainDb;
        if (!std::isfinite(total)) total = 0.0f;
    }

    float attnsF = dspAttnsFactor(p);
    if (attnsF != 1.0f) total += dspLinToDb(attnsF);

    float mic = p.micGainLin;
    if (!(mic > 1e-6f) || !std::isfinite(mic)) mic = 1.0f;
    total += dspLinToDb(mic);

    if (p.asroutEnabled) {
        float asr = p.asroutGainLin;
        if (!(asr > 1e-6f) || !std::isfinite(asr)) asr = 1.0f;
        total += dspLinToDb(asr);
    }

    float sw = p.softwareGainLin;
    if (!(sw > 1e-6f) || !std::isfinite(sw)) sw = 1.0f;
    if (sw != 1.0f) total += dspLinToDb(sw);
    return total;
}

inline float dspCompensateLevelDb(float rawDb, float effectiveGainDb) {
    if (!std::isfinite(rawDb)) return rawDb;
    float g = std::isfinite(effectiveGainDb) ? effectiveGainDb : 0.0f;
    return rawDb - g;
}

#endif /* DSP_LEVEL_COMPENSATE_H */
