/**
 * @file    RTSPClient_stub.cpp
 * @brief   Stub RTSPClient: isStreaming из RtspMicTest::g_rtspStreaming для telemetry.
 */

#include "RTSPClient.h"

namespace RtspMicTest {
bool g_rtspStreaming = false;
}

RTSPClient::RTSPClient()
    : _fanout(nullptr)
    , _state(RTSPClientState::DISCONNECTED)
    , _stateMutex(nullptr)
    , _running(false)
    , _paused(false)
    , _port(0)
    , _ssrc(0)
    , _seqNum(0)
    , _rtpTimestamp(0)
    , _lastAudioTimestampMs(0)
    , _audioTimestampInitialized(false)
    , _backoff(1000, 30000)
{
    memset(_host, 0, sizeof(_host));
    memset(_sessionId, 0, sizeof(_sessionId));
}

void RTSPClient::setDependencies(EncodedAudioFanout *) {}
void RTSPClient::setServer(const char *host, uint16_t port) {
    strncpy(_host, host ? host : "", sizeof(_host) - 1);
    _port = port;
}
bool RTSPClient::begin() { return true; }
void RTSPClient::stop() {}
void RTSPClient::pauseAudio() { _paused = true; }
void RTSPClient::resumeAudio() { _paused = false; }
RTSPClientState RTSPClient::getState() const {
    return RtspMicTest::g_rtspStreaming ? RTSPClientState::PLAYING : RTSPClientState::DISCONNECTED;
}
bool RTSPClient::isStreaming() const { return RtspMicTest::g_rtspStreaming; }
