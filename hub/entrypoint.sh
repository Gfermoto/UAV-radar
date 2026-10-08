#!/bin/sh
set -eu
PORT="${HUB_PORT:-${MOCK_PORT:-9443}}"
ARGS="--bind ${HUB_BIND:-${MOCK_BIND:-0.0.0.0}} --port ${PORT} --cert-dir ${HUB_CERT_DIR:-${MOCK_CERT_DIR:-/data/certs}}"
if [ -n "${INGEST_TOKEN:-}" ]; then
  ARGS="$ARGS --token ${INGEST_TOKEN}"
fi
CN="${HUB_CN:-${MOCK_CN:-}}"
if [ -n "$CN" ]; then
  ARGS="$ARGS --cn ${CN}"
fi
case "${HTTP_CLEARTEXT:-0}" in
  1|true|TRUE|yes|YES)
    exec python3 -u /app/nevod_hub.py --http $ARGS
    ;;
  *)
    exec python3 -u /app/nevod_hub.py --https $ARGS
    ;;
esac
