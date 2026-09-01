#include <unity.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "AudioProducer.h"
#include "EncodedAudioFanout.h"
#include "OpusEncoder.h"
#include "RTSPClient.h"
#include "RTSPServer.h"
#include "WebCredentials.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "WiFi.h"

namespace {

template <typename Predicate>
bool waitUntil(Predicate predicate, uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

void test_actual_audio_pipeline_preserves_partial_pcm() {
    test_i2s_reset();
    AudioProducer producer;
    EncodedAudioFanout fanout(8);
    OpusEncoderTask encoder;

    TEST_ASSERT_TRUE(producer.begin());
    TEST_ASSERT_TRUE(encoder.begin(&producer, &fanout));

    std::vector<int32_t> stereo(1024, 1000 << 16);
    test_i2s_push(stereo.data(), stereo.size());
    test_i2s_push(stereo.data(), stereo.size());

    TEST_ASSERT_TRUE(waitUntil([&]() {
        return fanout.stats(EncodedAudioConsumer::LOCAL_RTSP).enqueued >= 3;
    }, 1500));

    auto local = fanout.stats(EncodedAudioConsumer::LOCAL_RTSP);
    auto remote = fanout.stats(EncodedAudioConsumer::REMOTE_RTSP);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3, local.enqueued);
    TEST_ASSERT_EQUAL_UINT32(local.enqueued, remote.enqueued);

    encoder.stop();
    producer.stop();
}

void test_actual_rtsp_client_reconnect_uses_post_connect_time() {
    fake_wifi_reset();
    test_millis_set(0);
    fake_wifi_set_connect_result(false, 250);

    EncodedAudioFanout fanout(4);
    RTSPClient client;
    client.setDependencies(&fanout, nullptr);
    client.setServer("127.0.0.1", 8555);
    TEST_ASSERT_TRUE(client.begin());

    TEST_ASSERT_TRUE(waitUntil(
        []() { return fake_wifi_connect_attempt_count() == 1; }, 700));
    TEST_ASSERT_EQUAL_UINT32(0, fake_wifi_connect_attempt_time(0));

    test_millis_set(2249);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    TEST_ASSERT_EQUAL_UINT32(1, fake_wifi_connect_attempt_count());

    test_millis_set(2250);
    TEST_ASSERT_TRUE(waitUntil(
        []() { return fake_wifi_connect_attempt_count() == 2; }, 700));
    client.stop();
}

void test_actual_rtsp_server_state_is_locked_without_network_under_mutex() {
    fake_wifi_reset();
    test_millis_set(0);
    // Default password is rejected (403); seed a rotated credential for auth.
    TEST_ASSERT_TRUE(WebCredentials::save("admin", "testpass1"));
    // base64("admin:testpass1")
    static const char *kAuth = "Authorization: Basic YWRtaW46dGVzdHBhc3Mx\r\n";
    const std::string setupPlay =
        std::string("SETUP rtsp://node/trackID=0 RTSP/1.0\r\nCSeq: 1\r\n") + kAuth + "\r\n"
        "PLAY rtsp://node/ RTSP/1.0\r\nCSeq: 2\r\n" + kAuth + "\r\n";

    WiFiClient socket = fake_wifi_make_client(setupPlay.c_str());
    fake_wifi_server_enqueue(socket);

    EncodedAudioFanout fanout(4);
    RTSPServer server;
    server.setDependencies(&fanout, nullptr);
    TEST_ASSERT_TRUE(server.begin());
    TEST_ASSERT_TRUE(waitUntil(
        [&]() { return server.getActiveClientCount() == 1; }, 700));

    std::atomic<bool> invalidCount{false};
    std::thread reader([&]() {
        for (int i = 0; i < 500; ++i) {
            if (server.getActiveClientCount() > 1) invalidCount = true;
        }
    });
    const std::string teardown =
        std::string("TEARDOWN rtsp://node/ RTSP/1.0\r\nCSeq: 3\r\n") + kAuth + "\r\n";
    socket.appendIncoming(teardown.c_str());
    TEST_ASSERT_TRUE(waitUntil(
        [&]() { return server.getActiveClientCount() == 0; }, 700));
    reader.join();

    server.stop();
    TEST_ASSERT_FALSE(invalidCount.load());
    TEST_ASSERT_EQUAL_UINT32(0, fake_wifi_network_calls_under_mutex());
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_actual_audio_pipeline_preserves_partial_pcm);
    RUN_TEST(test_actual_rtsp_client_reconnect_uses_post_connect_time);
    RUN_TEST(test_actual_rtsp_server_state_is_locked_without_network_under_mutex);
    return UNITY_END();
}
