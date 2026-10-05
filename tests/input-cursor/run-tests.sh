#!/usr/bin/env bash
set -euo pipefail
repo_path=$(cd "$(dirname "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cd "$test_dir"
qmake6 "$repo_path/tests/input-cursor/cursor.pro"
make -j4
./cursor-tests
