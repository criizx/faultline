#!/usr/bin/env bash
set -euo pipefail

if (($# != 5)); then
    echo "usage: $0 RESULTS MAX_LATENCY_ERROR_MS MAX_BANDWIDTH_ERROR_PERCENT MIN_PROXY_MBPS MIN_CONNECTIONS_PER_SECOND" >&2
    exit 2
fi

results="$1"
max_latency_error_ms="$2"
max_bandwidth_error_percent="$3"
min_proxy_mbps="$4"
min_connections_per_second="$5"

jq -s -e \
    --argjson max_latency "$max_latency_error_ms" \
    --argjson max_bandwidth "$max_bandwidth_error_percent" \
    --argjson min_proxy "$min_proxy_mbps" \
    --argjson min_churn "$min_connections_per_second" '
    def magnitude: if . < 0 then -. else . end;
    ([.[] | select(.benchmark == "latency_accuracy") | .injected_error_ms] |
        length > 0 and all(.[]; magnitude <= $max_latency)) and
    ([.[] | select(.benchmark == "bandwidth_accuracy") | .error_percent] |
        length > 0 and all(.[]; magnitude <= $max_bandwidth)) and
    ([.[] | select(.benchmark == "proxy_overhead") | .proxy_mbps] |
        length > 0 and all(.[]; . >= $min_proxy)) and
    ([.[] | select(.benchmark == "connection_churn") | .connections_per_second] |
        length > 0 and all(.[]; . >= $min_churn))
    ' "$results" >/dev/null
