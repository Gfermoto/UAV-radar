#pragma once

#include <cstdint>
#include <cstring>

struct OpusEncoder {
    int sampleRate;
};

#define OPUS_OK 0
#define OPUS_APPLICATION_AUDIO 2049
#define OPUS_SET_BITRATE(value) 4002, (value)
#define OPUS_SET_COMPLEXITY(value) 4010, (value)
#define OPUS_SET_VBR(value) 4006, (value)
#define OPUS_SET_DTX(value) 4016, (value)
#define OPUS_SET_INBAND_FEC(value) 4012, (value)
#define OPUS_SET_PACKET_LOSS_PERC(value) 4014, (value)

inline OpusEncoder *opus_encoder_create(int sampleRate, int, int, int *error) {
    if (error) *error = OPUS_OK;
    return new OpusEncoder{sampleRate};
}

inline void opus_encoder_destroy(OpusEncoder *encoder) {
    delete encoder;
}

inline int opus_encoder_ctl(OpusEncoder *, int, ...) {
    return OPUS_OK;
}

inline int opus_encode(OpusEncoder *, const int16_t *pcm, int frameSize,
                       uint8_t *output, int capacity) {
    if (!pcm || !output || frameSize <= 0 || capacity < 8) return -1;
    int32_t checksum = 0;
    for (int i = 0; i < frameSize; ++i) checksum += pcm[i];
    std::memcpy(output, &checksum, sizeof(checksum));
    std::memcpy(output + sizeof(checksum), &frameSize, sizeof(frameSize));
    return 8;
}
