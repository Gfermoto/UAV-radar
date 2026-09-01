/**
 * @file    Config.h
 * @brief   Общие константы прошивки RTSP-микрофона (XIAO ESP32-S3 + XVF3800).
 *
 * Пины, приоритеты задач, MQTT/RTSP/Opus/MEL, дефолты WebUI.
 * Профиль сборки: ConfigDev.h. Карта системы: docs/ARCHITECTURE.md.
 * Каталог MQTT/NVS: docs/API_REFERENCE.md. LED: docs/LED.md.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "ConfigDev.h"

// =============================================================================
//  Локальный порт RTSP (runtime; по умолчанию 554)
// =============================================================================

/** Хранилище listen-порта (изменяется до start сервера). */
inline uint16_t &rtspLocalPortStorage() {
    static uint16_t port = 554;
    return port;
}

inline uint16_t rtspLocalPort() { return rtspLocalPortStorage(); }

/** port==0 игнорируется. */
inline void rtspSetLocalPort(uint16_t port) {
    if (port == 0) return;
    rtspLocalPortStorage() = port;
}

// =============================================================================
//  Идентификация узла
// =============================================================================

#define NODE_ID_LEN            7
#define NODE_ID_FMT            "%02X%02X%02X"

// =============================================================================
//  Сетевые настройки (общие)
// =============================================================================

#define WIFI_CONNECT_TIMEOUT_MS   30000
#define WIFI_RETRY_MAX            5
#define WIFI_CHECK_INTERVAL_MS     3000
/** Legacy compile-time default; runtime hostname = WiFiSetup::hostname() (RM+MAC). */
#define MDNS_HOSTNAME             "rtsp-mic"  // не путать с WiFiSetup::hostname()
#define DEVICE_ID_LEN             11  ///< "RM" + 8 hex + NUL
#define AP_SSID_PREFIX            "RTSPMIC-"

// =============================================================================
//  Пины I2S
// =============================================================================

#define PIN_I2S_BCLK     GPIO_NUM_8
#define PIN_I2S_WS       GPIO_NUM_7
#define PIN_I2S_DATA_IN  GPIO_NUM_43

#define I2S_DMA_BUF_LEN          512
#define I2S_DMA_BUF_COUNT        4
#define I2S_BITS_PER_SAMPLE      I2S_BITS_PER_SAMPLE_32BIT
#define I2S_SAMPLE_RATE          16000
#define MEL_FFT_SIZE             512

// =============================================================================
//  Пины I2C
// =============================================================================

// XIAO ESP32-S3: D4=GPIO5 (SDA), D5=GPIO6 (SCL) — см. Seeed ReSpeaker XVF3800.
#define PIN_I2C_SDA      GPIO_NUM_5
#define PIN_I2C_SCL      GPIO_NUM_6
#define XVF3800_I2C_ADDR 0x2C
#define I2C_CLOCK_FREQ   50000

// =============================================================================
//  Пины — LED + Ethernet
// =============================================================================

/** Статусный LED XIAO; не RGB-кольцо микрофона (docs/LED.md). */
#define PIN_LED_STATUS   GPIO_NUM_21

#define PIN_ETH_CS       GPIO_NUM_10
#define PIN_ETH_INT      GPIO_NUM_11
#define PIN_ETH_RST      GPIO_NUM_12
#define PIN_SPI_SCK      GPIO_NUM_13
#define PIN_SPI_MOSI     GPIO_NUM_15
#define PIN_SPI_MISO     GPIO_NUM_14

// =============================================================================
//  XVF3800
// =============================================================================

#define RESID_GPO_SERVICER   20
#define RESID_AEC            33
#define RESID_VERSION        48
#define RESID_PP             17
#define RESID_AUDIO_MGR      35

// AEC (ResID 33) — официальная карта xvf_host / XMOS UG v3.2.1
#define CMD_HPFONOFF           1   ///< AEC_HPFONOFF
#define CMD_SILENCELEVEL       2   ///< AEC_AECSILENCELEVEL
#define CMD_EMPHASIS           4   ///< AEC_AECEMPHASISONOFF
#define CMD_ASROUT            35   ///< AEC_ASROUTONOFF
#define CMD_ASROUTGAIN        36   ///< AEC_ASROUTGAIN
#define CMD_FIXEDBEAMSONOFF   37   ///< AEC_FIXEDBEAMSONOFF
#define CMD_FIXEDBEAMNOISETHR 38
#define CMD_AZIMUTH           75   ///< AEC_AZIMUTH_VALUES (4×float radians)
#define CMD_SPENERGY          80   ///< AEC_SPENERGY_VALUES
#define CMD_FIXEDBEAMSAZ      81   ///< AEC_FIXEDBEAMSAZIMUTH_VALUES
#define CMD_FIXEDBEAMSEL      82
#define CMD_FIXEDBEAMSGATING  83
#define CMD_AECCONVERGED       3
#define CMD_RT60               9
#define CMD_MIC_ARRAY_TYPE    73
#define CMD_MIC_ARRAY_GEO     74
#define CMD_SHF_BYPASS        70

// GPO (ResID 20)
#define CMD_GPO_WRITE_VALUE    1   ///< GPO_WRITE_VALUE: payload [pin, level]
/** Seeed reSpeaker X0D30 — mute circuit + red LED (HIGH=mute). Wiki/host_control. */
#define XVF_GPO_MUTE_PIN      30
/**
 * Seeed reSpeaker RGB ring (WS2812×12) via XVF GPO servicer (ResID 20).
 * host_control LED_EFFECT: 0=off 1=breath 2=rainbow 3=color 4=doa.
 * Product «Кольцо Вкл» = DoA only — do not force 1/2/3 (overrides factory DoA).
 * @see docs/LED.md, https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/host_control/README.md
 */
#define CMD_GPO_LED_EFFECT     12
#define CMD_GPO_LED_BRIGHTNESS 13  ///< unused by simple on/off path
#define CMD_GPO_LED_SPEED      15
#define CMD_GPO_LED_COLOR      16
#define XVF_LED_EFFECT_OFF     0
#define XVF_LED_EFFECT_DOA     4   ///< factory default after boot
#define CMD_READ_DOA          18   ///< DOA_VALUE: az + speech_detected
#define CMD_READ_VERSION       0
#define CMD_REBOOT             7

// AUDIO_MGR (ResID 35)
#define CMD_MIC_GAIN           0   ///< AUDIO_MGR_MIC_GAIN
#define CMD_REF_GAIN           1
#define CMD_SELECTED_AZIMUTHS 11   ///< AUDIO_MGR_SELECTED_AZIMUTHS
#define CMD_OP_L              15   ///< AUDIO_MGR_OP_L (cat, src)
#define CMD_OP_R              19   ///< AUDIO_MGR_OP_R (cat, src)
#define CMD_SYS_DELAY         26
/** Mux: cat6=processed beams, src3=auto-select (XMOS recommended path). */
#define XVF_OP_CAT_PROCESSED   6
#define XVF_OP_SRC_AUTOSELECT  3
#define XVF_OP_CAT_SILENCE     0

// PP (ResID 17)
#define PP_AGCONOFF            10
#define PP_AGCMAXGAIN          11
#define PP_AGCDESIREDLEVEL     12
#define PP_AGCGAIN             13   ///< current AGC linear gain (read)
#define PP_AGCTIME             14
#define PP_AGCFASTTIME         15
#define PP_AGCALPHASLOW        17
#define PP_AGCALPHAFAST        18
#define PP_LIMITONOFF          19
#define PP_LIMITPLIMIT         20
#define PP_MIN_NS              21
#define PP_MIN_NN              22
#define PP_ECHOONOFF           23
#define PP_ATTNS_MODE          32
#define PP_ATTNS_NOMINAL       33
#define PP_ATTNS_SLOPE         34

#define I2C_READ_BIT          0x80
#define HTTP_EVENT_MAX_RETRIES 5
#define MQTT_CMD_MAX_AGE_MS    60000

// =============================================================================
//  Буферы
// =============================================================================

#define RING_BUFFER_SIZE       32768
#define NET_CHUNK_SAMPLES      512

// =============================================================================
//  NTP
// =============================================================================

#define NTP_TIMEOUT_MS         10000
#define NTP_DEFAULT_SERVER     "pool.ntp.org"
#define NTP_MIN_VALID_EPOCH    1609459200
#define NTP_RESYNC_INTERVAL_MS 3600000
#define NTP_AUTO_RECOVERY_MS   60000

// =============================================================================
//  Временные интервалы
// =============================================================================

#define WIFI_RECONNECT_TIMEOUT_MS  20000
#define WIFI_DEAD_REBOOT_MS        180000  ///< Wi-Fi отвалился >3 мин → reboot
#define RTSP_IDLE_TIMEOUT_MS      30000  ///< было 70000 — быстрее освобождаем TCP PCB
#define RTSP_CHUNK_INTERVAL_MS    5
#define SERIAL_BAUD_RATE          115200
#define STATS_PRINT_INTERVAL_MS   30000
#define ETH_SPI_FREQUENCY         4000000
#define XVF_POLL_INTERVAL_MS        100
#define EVENT_QUEUE_MAX      100
#define AUDIO_QUEUE_DEPTH     4

// =============================================================================
//  Приоритеты задач FreeRTOS
// =============================================================================

#define AUDIO_PRODUCER_PRIORITY   10
#define NETWORK_CONTROL_PRIORITY   5
#define AUDIO_PRODUCER_STACK_SIZE 8192
#define NETWORK_TASK_STACK_SIZE   4096
/** Opus: ~48KB FreeRTOS stack (CELT); одного PSRAM pseudostack мало. */
#define OPUS_TASK_STACK_SIZE      49152
/** WebUI: JSON телеметрии — 4KB стека давал overflow/reboot. */
#define WEB_UI_TASK_STACK_SIZE    12288
#define TASK_STOP_GRACE_MS        100

/** BOOT (GPIO0). Удержание ≥ FACTORY_RESET_HOLD_MS → factory reset. */
#define PIN_BTN_FACTORY_RESET     GPIO_NUM_0
/** 3s hold. Ложный LOW от USB-CDC — disarm в WiFiSetup, не укорачивать hold. */
#define FACTORY_RESET_HOLD_MS           3000
#define FACTORY_RESET_STABLE_HIGH_MS     200
#define FACTORY_RESET_WIFI_SUPPRESS_MS  15000

// =============================================================================
//  Аудио
// =============================================================================

#define DEFAULT_GAIN          1.0f
#define PCM_MAX_AMP           32767
#define PCM_MIN_AMP          -32768
#define PCM_SCALE_F           32768.0f
#define I2S_ERROR_LOG_INTERVAL_MS  30000

#ifndef OPUS_ENABLED
#define OPUS_ENABLED          1
#endif

// MQTT — шаблоны топиков; {node_id} подставляется в runtime (docs/API_REFERENCE.md)
#define MQTT_KEEPALIVE_SEC    30
#define MQTT_CONNECT_TIMEOUT_MS  10000
#define MQTT_TOPIC_TELEMETRY  "rtsp-mic/v1/{node_id}/telemetry"
#define MQTT_TOPIC_STATUS     "rtsp-mic/v1/{node_id}/status"
#define MQTT_TOPIC_EVENTS     "rtsp-mic/v1/{node_id}/events"
#define MQTT_TOPIC_COMMAND    "rtsp-mic/v1/{node_id}/command"

// HTTP
#define HTTP_TIMEOUT_MS       5000
#define HTTP_RETRY_MAX        3
#define HTTP_RETRY_BASE_MS    1000

// RTSP/RTP
#define RTSP_BUF_SIZE         2048
#define RTSP_MAX_CLIENTS      4    ///< было 10 — жёстко: меньше TCP PCB на каждого клиента
/** CONNECTED без SETUP: cleanupIdleSessions закрывает слот (probe/OPTIONS). */
#define RTSP_CONNECT_TIMEOUT_MS  2000
#define RTP_CHANNEL           0x00
#define RTP_VERSION           0x80
#define RTP_PAYLOAD_TYPE      96
#define OPUS_RTP_CLOCK_RATE    48000
#define OPUS_RTP_SDP_CHANNELS  1
#define OPUS_RTP_TIMESTAMP_STEP (OPUS_RTP_CLOCK_RATE * OPUS_FRAME_MS / 1000)
#define ENCODED_AUDIO_QUEUE_DEPTH 4
#define RTP_HEADER_SIZE       12
#define RTP_INTERLEAVE_PREFIX 4
#define RTP_INTERLEAVE_MARKER 0x24

// Web
#define WS_ENDPOINT           "/ws"
#define WS_UPDATE_INTERVAL_MS 1000
#define REBOOT_DELAY_MS         3000
#define RTSP_RESPONSE_TIMEOUT_MS 2000

// LED
#define LED_BLINK_STARTUP_MS    200
#define LED_BLINK_ERROR_ON_MS   100
#define LED_BLINK_ERROR_OFF_MS  100
#define LED_BLINK_NET_FAIL_MS   1000
#define LED_BLINK_WARNING_ON_MS 200
#define LED_BLINK_WARNING_OFF_MS 200
#define LED_BLINK_ALARM_MS      100

#define TICK_DELAY_MS(ms)     vTaskDelay(pdMS_TO_TICKS(ms))

// MEL
#define MEL_NUM_BANDS         64
#define MEL_WINDOW_LENGTH     (I2S_SAMPLE_RATE * 25 / 1000)

#if MEL_WINDOW_LENGTH > MEL_FFT_SIZE
#error "MEL_WINDOW_LENGTH > MEL_FFT_SIZE"
#endif

// 13ms @ 48kHz
#define MEL_HOP_LENGTH        (I2S_SAMPLE_RATE * 10 / 1000)
#define MEL_NUM_FRAMES        401
#define MEL_FMIN              125
#define MEL_FMAX              7500

// Opus
#define OPUS_SAMPLE_RATE      I2S_SAMPLE_RATE
#define OPUS_FRAME_MS         20
#define OPUS_FRAME_SIZE       (OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000)
#define OPUS_BITRATE          32000
// Complexity 5 — хорошее качество для птиц/речи на 16 kHz mono.
// Выше 5 — alloca-кодер не отпускает IDLE0 → task_wdt panic на ESP32-S3.
// Предупреждение от авторов Opus: pseudostack на alloca без yield опасен >5.
#define OPUS_COMPLEXITY       5
#define OPUS_PSEUDOSTACK_SIZE 122880

// AP
#define AP_IP_ADDRESS         "192.168.4.1"
#define AP_PORTAL_TIMEOUT_MS  180000
#ifndef AP_PASSWORD
#define AP_PASSWORD           ""
#endif
#ifndef WEB_UI_USER
#define WEB_UI_USER           "admin"
#endif
#ifndef WEB_UI_DEFAULT_PASSWORD
#define WEB_UI_DEFAULT_PASSWORD "rtsp-mic-change-me"
#endif
#ifndef WEB_UI_PASSWORD
#define WEB_UI_PASSWORD WEB_UI_DEFAULT_PASSWORD
#endif

#endif // CONFIG_H