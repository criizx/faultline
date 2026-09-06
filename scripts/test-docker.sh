#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

compose=(docker compose -f compose.yml -f compose.demo.yml)

trap 'docker compose -f compose.yml -f compose.demo.yml down --volumes' EXIT
"${compose[@]}" up --build --detach

for _ in {1..60}; do
    if health="$(curl --fail --silent --show-error http://127.0.0.1:4173/api/healthz 2>/dev/null)" &&
        [[ "$health" == *'"status":"ok"'* ]] &&
        response="$(python3 demo/client.py docker-smoke 2>/dev/null)" &&
        [[ "$response" == response=docker-smoke* ]]; then
        printf '%s\n' "$response"
        exit 0
    fi
    sleep 1
done

"${compose[@]}" ps
"${compose[@]}" logs >&2
exit 1
