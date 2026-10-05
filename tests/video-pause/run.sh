#!/usr/bin/env bash
set -euo pipefail
repo_path=$(cd "$(dirname "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -pthread -g -O1 \
    -I"$repo_path/app" -I"$repo_path/moonlight-common-c/moonlight-common-c/src" \
    $(pkg-config --cflags Qt6Core Qt6Gui Qt6Qml sdl2 SDL2_ttf libavcodec libavutil) \
    "$repo_path/tests/video-pause/pause_test.cpp" \
    "$repo_path/app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp" \
    $(pkg-config --libs Qt6Core Qt6Gui Qt6Qml sdl2 SDL2_ttf libavcodec libavutil) \
    -o "$test_dir/video-pause-test"
"$test_dir/video-pause-test"
