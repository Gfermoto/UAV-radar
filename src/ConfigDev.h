/**
 * @file    ConfigDev.h
 * @brief   Профиль прошивки RTSP mic — XVF3800 DSP → Opus/RTSP (без NN).
 *
 * Ethernet compiled in. MQTT = опциональная телеметрия. Web :80, RTSP :554.
 *
 * @see Config.h, docs/ARCHITECTURE.md
 */
#ifndef CONFIG_DEV_H
#define CONFIG_DEV_H

#define FIRMWARE_NAME          "RTSP Mic"
#define FIRMWARE_VERSION       "0.3.0"
#define NODE_TYPE_STR          "mic"

#define RTSP_LOCAL_PORT        554
#define WEB_PORT               80
#define MQTT_LOCAL_PORT        1883
#define RTSP_REMOTE_PORT       554

#define ETHERNET_ENABLED       1
#define CERT_CHAIN_ENABLED     0

#endif // CONFIG_DEV_H
