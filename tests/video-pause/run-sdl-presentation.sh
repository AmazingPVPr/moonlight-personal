#!/usr/bin/env bash
set -euo pipefail
repo_path=$(cd "$(dirname "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
test_packages="Qt6Core Qt6Gui Qt6Qml Qt6Quick sdl2 SDL2_ttf opus libavcodec libavutil libswscale"
"${CXX:-c++}" -std=c++17 -pthread -fPIC -mno-direct-extern-access -g -O1 \
    -I"$repo_path/app" -I"$repo_path/moonlight-common-c/moonlight-common-c/src" \
    -I"$repo_path/qmdnsengine/qmdnsengine/src/include" -I"$repo_path/qmdnsengine" \
    $(pkg-config --cflags $test_packages) \
    "$repo_path/tests/video-pause/sdl_presentation_test.cpp" \
    "$repo_path/app/streaming/video/ffmpeg-renderers/sdlvid.cpp" \
    "$repo_path/app/streaming/video/ffmpeg-renderers/swframemapper.cpp" \
    $(pkg-config --libs $test_packages) -ldl -o "$test_dir/sdl-presentation-test"
SDL_VIDEODRIVER=x11 "$test_dir/sdl-presentation-test"
