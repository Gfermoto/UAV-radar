/**
 * @file    AudioProducer_stub.cpp
 * @brief   Минимальный AudioProducer для native-тестов SystemMonitor/Liveness.
 *          Управляемые mock_* — телеметрия, ringbuf, begin/stop без I2S.
 */

#include "AudioProducer.h"
#include <cstring>

// Mock controls
static AudioTelemetry mock_telemetry;
static size_t        mock_available      = 0;
static RingbufHandle_t mock_ringbuf      = reinterpret_cast<RingbufHandle_t>(1);
static bool          mock_begin_success  = true;
static int           mock_stop_calls     = 0;
static int           mock_begin_calls    = 0;

void mock_audio_set_telemetry(const AudioTelemetry &t) { mock_telemetry = t; }
void mock_audio_set_available(size_t avail)             { mock_available = avail; }
void mock_audio_set_ringbuf(void *ptr)                  { mock_ringbuf = static_cast<RingbufHandle_t>(ptr); }
void mock_audio_set_begin_result(bool ok)               { mock_begin_success = ok; }
int  mock_audio_get_stop_calls()   { return mock_stop_calls; }
int  mock_audio_get_begin_calls()  { return mock_begin_calls; }

void mock_audio_reset() {
    std::memset(&mock_telemetry, 0, sizeof(mock_telemetry));
    mock_available     = 0;
    mock_ringbuf       = reinterpret_cast<RingbufHandle_t>(1);
    mock_begin_success = true;
    mock_stop_calls    = 0;
    mock_begin_calls   = 0;
}

// ---- Stub реализация AudioProducer ----

AudioProducer::AudioProducer() { _ringBuf = nullptr; }
void AudioProducer::getTelemetry(AudioTelemetry &t) { t = mock_telemetry; }
size_t AudioProducer::available() const { return mock_available; }
RingbufHandle_t AudioProducer::getRingBuffer() const { return mock_ringbuf; }
void AudioProducer::stop() { mock_stop_calls++; }
bool AudioProducer::begin() { mock_begin_calls++; return mock_begin_success; }
void AudioProducer::setGain(float) {}
float AudioProducer::getGain() { return 1.2f; }
void AudioProducer::setHpfMode(uint8_t) {}
void AudioProducer::setHpfEnabled(bool) {}
bool AudioProducer::isHpfEnabled() const { return false; }
uint8_t AudioProducer::getHpfMode() const { return 0; }
float AudioProducer::getHpfCutoff() const { return 0.0f; }
