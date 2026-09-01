/**
 * @file    XVF3800_I2C_stub.cpp
 * @brief   Управляемый mock XVF3800 (RtspMicTest::g_*) для native-тестов cache/telemetry.
 */

#include "XVF3800_I2C.h"
#include "NetConfig.h"

namespace RtspMicTest {
bool     g_vadActive     = false;
uint16_t g_doaAzimuth    = 180;
float    g_speechDetected = 0.85f;
XVF3800_Result g_vadResult = XVF3800_Result::OK;
XVF3800_Result g_doaResult = XVF3800_Result::OK;
bool     g_initialized     = true;
float    g_spEnergy[4]     = {0.1f, 0.05f, 0.08f, 0.12f};
float    g_beamAzimuthDeg[4] = {90.0f, 200.0f, 90.0f, 90.0f};
float    g_selectedAzDeg   = 180.0f;
float    g_autoSelectAzDeg = 180.0f;
bool     g_selectedValid   = true;
bool     g_aecConverged    = true;
float    g_rt60            = 0.35f;
int32_t  g_micArrayType    = 2;
}

bool XVF3800_I2C::begin() {
    RtspMicTest::g_initialized = true;
    return true;
}

bool XVF3800_I2C::isConnected() {
    return RtspMicTest::g_initialized;
}

bool XVF3800_I2C::recoverBus() {
    return RtspMicTest::g_initialized;
}

XVF3800_Result XVF3800_I2C::readDOA(uint16_t &azimuth, float &confidence) {
    if (!RtspMicTest::g_initialized) return XVF3800_Result::ERR_NOT_INIT;
    if (RtspMicTest::g_doaResult != XVF3800_Result::OK) return RtspMicTest::g_doaResult;
    azimuth = RtspMicTest::g_doaAzimuth;
    // Как на железе: speech_detected 0|1 (g_vadActive); g_speechDetected — зеркало.
    confidence = RtspMicTest::g_vadActive ? 1.0f : 0.0f;
    RtspMicTest::g_speechDetected = confidence;
    return XVF3800_Result::OK;
}

XVF3800_Result XVF3800_I2C::readVAD(bool &active) {
    if (!RtspMicTest::g_initialized) return XVF3800_Result::ERR_NOT_INIT;
    if (RtspMicTest::g_vadResult != XVF3800_Result::OK) return RtspMicTest::g_vadResult;
    active = RtspMicTest::g_vadActive;
    return XVF3800_Result::OK;
}

XVF3800_Result XVF3800_I2C::writeMute(bool) { return XVF3800_Result::OK; }
/** Host stub: no I2C; production writes LED_EFFECT 0/4 (docs/LED.md). */
XVF3800_Result XVF3800_I2C::setLedRingEnabled(bool) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setAGC(bool) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::readAGCGain(float &gainDb) {
    gainDb = 0.0f;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::setEchoSuppression(bool) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setFixedBeam(XVF3800_BeamMode) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setXvfHpfMode(uint8_t) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::readXvfHpfMode(uint8_t &mode) {
    mode = 2;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::setEmphasis(uint8_t) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setMicGain(float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setSilenceLevel(float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::readSpEnergy(float &b0, float &b1, float &b2, float &b3) {
    if (!RtspMicTest::g_initialized) return XVF3800_Result::ERR_NOT_INIT;
    b0 = RtspMicTest::g_spEnergy[0];
    b1 = RtspMicTest::g_spEnergy[1];
    b2 = RtspMicTest::g_spEnergy[2];
    b3 = RtspMicTest::g_spEnergy[3];
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::readAzimuthsDeg(float deg[4]) {
    if (!RtspMicTest::g_initialized) return XVF3800_Result::ERR_NOT_INIT;
    if (!deg) return XVF3800_Result::ERR_BAD_LENGTH;
    for (int i = 0; i < 4; i++) deg[i] = RtspMicTest::g_beamAzimuthDeg[i];
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::setAsrout(bool) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setAsroutGain(float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::readSelectedAzimuthsDeg(float &processedDeg, float &autoSelectDeg,
                                                    bool &processedValid) {
    if (!RtspMicTest::g_initialized) return XVF3800_Result::ERR_NOT_INIT;
    processedDeg = RtspMicTest::g_selectedAzDeg;
    autoSelectDeg = RtspMicTest::g_autoSelectAzDeg;
    processedValid = RtspMicTest::g_selectedValid;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::setFixedBeamAzimuthsDeg(float, float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setFixedBeamGating(bool) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setAttns(uint8_t, float, float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setRefGain(float) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setSysDelay(int32_t) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::setOutputMux(uint8_t, uint8_t, uint8_t, uint8_t) { return XVF3800_Result::OK; }
XVF3800_Result XVF3800_I2C::readAecConverged(bool &converged) {
    converged = RtspMicTest::g_aecConverged;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::readRt60(float &seconds) {
    seconds = RtspMicTest::g_rt60;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::readMicArrayType(int32_t &type) {
    type = RtspMicTest::g_micArrayType;
    return XVF3800_Result::OK;
}
bool XVF3800_I2C::applyEngineerConfig(const NetConfigData &) { return true; }
XVF3800_Result XVF3800_I2C::readVersion(uint8_t &major, uint8_t &minor, uint8_t &patch) {
    major = 1; minor = 0; patch = 0;
    return XVF3800_Result::OK;
}
XVF3800_Result XVF3800_I2C::reboot() { return XVF3800_Result::OK; }

const char *XVF3800_I2C::resultToString(XVF3800_Result result) {
    switch (result) {
        case XVF3800_Result::OK:           return "OK";
        case XVF3800_Result::ERR_TIMEOUT:  return "TIMEOUT";
        default:                           return "ERROR";
    }
}
