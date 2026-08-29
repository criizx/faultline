#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"

find "$repo_root/.githooks" "$repo_root/scripts" -type f -exec bash -n {} +

if command -v shellcheck >/dev/null 2>&1; then
    find "$repo_root/.githooks" "$repo_root/scripts" -type f -exec shellcheck {} +
fi

"$repo_root/scripts/check-format.sh"
"$repo_root/scripts/run-tests.sh"
