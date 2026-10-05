#!/usr/bin/env bash
set -euo pipefail
repo_path=$(cd "$(dirname "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
test_packages="Qt6Core Qt6Gui Qt6Qml Qt6Quick sdl2 SDL2_ttf opus libplacebo libavcodec libavutil vulkan"
"${CC:-cc}" -std=c11 -O1 $(pkg-config --cflags libplacebo libavcodec libavutil) \
    -c "$repo_path/app/streaming/video/ffmpeg-renderers/plvk_c.c" -o "$test_dir/plvk_c.o"
"${CXX:-c++}" -std=c++17 -pthread -fPIC -mno-direct-extern-access -g -O1 \
    -I"$repo_path/app" -I"$repo_path/moonlight-common-c/moonlight-common-c/src" \
    -I"$repo_path/qmdnsengine/qmdnsengine/src/include" -I"$repo_path/qmdnsengine" \
    $(pkg-config --cflags $test_packages) \
    "$repo_path/tests/video-pause/renderer_presentation_test.cpp" \
    "$repo_path/app/streaming/video/ffmpeg-renderers/plvk.cpp" \
    "$test_dir/plvk_c.o" $(pkg-config --libs $test_packages) \
    -o "$test_dir/renderer-presentation-test"
# An unmapped X11 window does not alter focus under the user's Wayland desktop.
SDL_VIDEODRIVER=x11 "$test_dir/renderer-presentation-test"
