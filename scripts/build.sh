#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/artifacts/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$PROJECT_ROOT/artifacts/build" --parallel 2
ctest --test-dir "$PROJECT_ROOT/artifacts/build" --output-on-failure
