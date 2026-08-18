#!/usr/bin/env bash
set -euo pipefail

RUN_DIR="/data/run/discord-http3"
RESTART_FLAG="${RUN_DIR}/restart.flag"

mkdir -p "${RUN_DIR}"

while true; do
  rm -f "${RESTART_FLAG}"
  python -m app.discord_http3 &
  RUNTIME_PID=$!

  while kill -0 "${RUNTIME_PID}" 2>/dev/null; do
    if [ -f "${RESTART_FLAG}" ]; then
      kill -TERM "${RUNTIME_PID}" 2>/dev/null || true
      wait "${RUNTIME_PID}" || true
      break
    fi
    sleep 2
  done

  wait "${RUNTIME_PID}" || true
  sleep 1
done
