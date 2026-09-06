#!/bin/sh
set -eu

config_path=/tmp/faultline.conf

printf '%s\n' \
  '[scenario]' \
  'name=compose' \
  "experiment_id=${FAULTLINE_EXPERIMENT_ID}" \
  'seed=42042' \
  '' \
  '[proxy]' \
  'listen_host=0.0.0.0' \
  'listen_port=8080' \
  "upstream_host=${FAULTLINE_UPSTREAM_HOST}" \
  "upstream_port=${FAULTLINE_UPSTREAM_PORT}" \
  '' \
  '[control]' \
  'host=0.0.0.0' \
  'port=9090' \
  'token=faultline-docker-demo' \
  '' \
  '[faults]' \
  'idle_timeout_ms=30000' \
  'reset_probability=0' \
  '' \
  '[upstream]' \
  "latency_ms=${FAULTLINE_UPSTREAM_LATENCY_MS:-0}" \
  "jitter_ms=${FAULTLINE_UPSTREAM_JITTER_MS:-0}" \
  "bandwidth_kbps=${FAULTLINE_UPSTREAM_BANDWIDTH_KBPS:-0}" \
  '' \
  '[downstream]' \
  "latency_ms=${FAULTLINE_DOWNSTREAM_LATENCY_MS:-0}" \
  "jitter_ms=${FAULTLINE_DOWNSTREAM_JITTER_MS:-0}" \
  "bandwidth_kbps=${FAULTLINE_DOWNSTREAM_BANDWIDTH_KBPS:-0}" > "$config_path"

exec faultline run --config "$config_path"
