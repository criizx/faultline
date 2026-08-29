#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

for llvm_bin in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
    if [[ -d "$llvm_bin" ]]; then
        PATH="$llvm_bin:$PATH"
        break
    fi
done

cmake -S . -B .build/checks \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFAULTLINE_BUILD_TESTS=ON \
    -DFAULTLINE_ENABLE_SANITIZERS="${FAULTLINE_SANITIZERS:-OFF}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .build/checks --parallel
ctest --test-dir .build/checks --output-on-failure

if command -v clang-tidy >/dev/null 2>&1; then
    cpp_sources=()
    while IFS= read -r -d '' file; do
        cpp_sources+=("$file")
    done < <(git ls-files --cached --others --exclude-standard -z -- \
        'src/*.cpp' 'tests/*.cpp')

    if ((${#cpp_sources[@]} > 0)); then
        clang_tidy_extra_args=()
        if [[ "$(uname -s)" == "Darwin" ]]; then
            clang_tidy_extra_args+=(
                --extra-arg=-isysroot
                "--extra-arg=$(xcrun --show-sdk-path)"
            )
        fi
        clang-tidy -p .build/checks "${cpp_sources[@]}" \
            --warnings-as-errors='bugprone-*,clang-analyzer-*,performance-*' \
            "${clang_tidy_extra_args[@]}"
    fi
fi
