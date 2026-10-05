#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/plvk.h"

#include <QCoreApplication>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

// This test uses synthetic pixels and an unmapped window, without a Session,
// host connection, decoder, or visible surface. The production Vulkan renderer
// has no overlays, so these stubs are never used to access a Session instance.
Session* Session::s_ActiveSession = nullptr;
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect*, SDL_Rect*) {}
extern "C" uint64_t LiGetMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_ERROR);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 77;
    auto window = SDL_CreateWindow("Video presentation regression test", 0, 0, 320, 180,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return 77; }
    bool available = false;
    {
        PlVkRenderer renderer;
        DECODER_PARAMETERS params = {};
        params.window = window;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.width = 320;
        params.height = 180;
        params.frameRate = 60;
        available = renderer.initialize(&params);
        if (available) {
            renderer.prepareToRender();
            // The initial black buffer maps the surface, but is not video.
            assert(renderer.presentedFrameSerial() == 0);
            AVFrame* frame = av_frame_alloc();
            assert(frame);
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = 320;
            frame->height = 180;
            frame->color_primaries = AVCOL_PRI_BT709;
            frame->color_trc = AVCOL_TRC_BT709;
            frame->colorspace = AVCOL_SPC_BT709;
            frame->color_range = AVCOL_RANGE_MPEG;
            assert(av_frame_get_buffer(frame, 32) == 0);
            for (int y = 0; y < frame->height; ++y) {
                std::memset(frame->data[0] + y * frame->linesize[0], 96, frame->width);
            }
            for (int plane = 1; plane < 3; ++plane) {
                for (int y = 0; y < frame->height / 2; ++y) {
                    std::memset(frame->data[plane] + y * frame->linesize[plane], 128, frame->width / 2);
                }
            }

            // No swapchain buffer has been acquired since initial-black submit.
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 0);

            renderer.waitToRender();
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 1);

            // Another failed call must not be counted as another video image.
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 1);
            av_frame_free(&frame);
            renderer.cleanupRenderContext();
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!available) return 77;
    std::puts("Production Vulkan serial counts one synthetic video image, excludes black startup and failed presentation calls.");
}
