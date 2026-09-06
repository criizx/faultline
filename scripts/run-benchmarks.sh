#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
profile="${1:-standard}"
build_dir="$repo_root/.build/benchmarks"

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFAULTLINE_BUILD_TESTS=OFF \
    -DFAULTLINE_BUILD_BENCHMARKS=ON >&2
cmake --build "$build_dir" --target faultline_benchmarks >&2
exec "$build_dir/faultline_benchmarks" "$profile"
