#!/usr/bin/env bash
set -euo pipefail

mkdir -p /data/run /data/run/discord-http3 /data/db /data/certs

python -m app.bootstrap

ARGS="--host ${WORKER_API_HOST:-0.0.0.0} --port ${WORKER_API_PORT:-8080}"
if [ -n "${WORKER_TLS_CERT_PATH:-}" ] && [ -n "${WORKER_TLS_KEY_PATH:-}" ]; then
  ARGS="$ARGS --ssl-certfile ${WORKER_TLS_CERT_PATH} --ssl-keyfile ${WORKER_TLS_KEY_PATH}"
fi

exec sh -lc "uvicorn app.worker_api:app $ARGS"
