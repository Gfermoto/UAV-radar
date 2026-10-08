# NEVOD Hub — панель участка. Контекст сборки = этот каталог.
# Заводской ключ прошивки в образ не кладётся: его задают в .env.
FROM python:3.12-slim-bookworm

LABEL org.opencontainers.image.source="https://github.com/Gfermoto/UAV-radar" \
      org.opencontainers.image.description="NEVOD Hub — on-prem site ingest" \
      org.opencontainers.image.licenses="MIT"

RUN apt-get update \
    && apt-get install -y --no-install-recommends openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir 'paho-mqtt>=2.1.0,<3' 'redis>=5.0.0,<6' 'numpy>=1.26,<3'

WORKDIR /app
COPY nevod_hub.py hub_diy_ui.html hub_login.html hub_store.py hub_webhooks.py hub_forward.py \
     hub_pb.py hub_mel_io.py hub_mel_invert.py hub_mel_gt.py generate_mel_lut.py hub_events.py hub_mqtt_parse.py \
     hub_auth.py hub_tls.py hub_episodes.py hub_context.py hub_runtime.py hub_labels.py \
     hub_http_auth_ui.py hub_http_mel.py hub_http_admin.py hub_http_mqtt.py hub_http_pb.py \
     hub_mqtt_mirror.py hub_mesh_adapter.py hub_self_test.py hub_zones.py hub_fog_bridge.py hub_redis.py \
     hub_overpass.py hub_meteo.py hub_ecowitt_share.py hub_dem.py hub_iso9613.py hub_map_wx.py \
     hub_bearing.py hub_tracks.py \
     hub_favicon.svg hub_favicon.ico /app/
COPY hub_ui /app/hub_ui
COPY adsb /app/adsb
COPY mock_cloud_diy.py /app/mock_cloud_diy.py
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV PYTHONUNBUFFERED=1 \
    HUB_BIND=0.0.0.0 \
    HUB_PORT=9443 \
    HUB_CERT_DIR=/data/certs \
    HUB_DATA_DIR=/data/hub \
    MEL_DIR=/data/mel \
    PRINT_DIY_CONFIG=0 \
    MOCK_BIND=0.0.0.0 \
    MOCK_PORT=9443 \
    MOCK_CERT_DIR=/data/certs \
    PYTHONDONTWRITEBYTECODE=1

VOLUME ["/data/certs", "/data/mel", "/data/hub"]
EXPOSE 9443

ENTRYPOINT ["/entrypoint.sh"]
