#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/plvk.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

#include <QCoreApplication>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

// Standalone renderer test: there is no Session, host, or video decoder. These
// unused renderer dependencies are replaced so the production Vulkan startup
// path can run without connecting to or disturbing an existing stream.
Session* Session::s_ActiveSession = nullptr;
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect*, SDL_Rect*) {}
int StreamUtils::getDisplayRefreshRate(SDL_Window*) { return 60; }
extern "C" uint64_t LiGetMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::atomic<int> initialBuffers{0};
static void logOutput(void*, int, SDL_LogPriority priority, const char* message)
{
    if (std::strstr(message, "Initial Vulkan window buffer presented before video decoding")) {
        initialBuffers++;
    }
    if (priority >= SDL_LOG_PRIORITY_WARN) {
        std::fprintf(stderr, "%s\n", message);
    }
}

class StartupRenderer : public PlVkRenderer
{
public:
    // Keep this standalone smoke test deterministic: no render thread can
    // acquire another swapchain buffer before the paused state is applied.
    bool isRenderThreadSupported() override { return false; }
    void renderFrame(AVFrame* frame) override
    {
        videoFrames++;
        PlVkRenderer::renderFrame(frame);
    }
    int videoFrames = 0;
};

static AVFrame* makeVideoImage()
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 320;
    frame->height = 180;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->pkt_dts = LiGetMicroseconds();
    assert(av_frame_get_buffer(frame, 32) == 0);
    for (int y = 0; y < frame->height; ++y) {
        std::memset(frame->data[0] + y * frame->linesize[0], 96, frame->width);
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < frame->height / 2; ++y) {
            std::memset(frame->data[plane] + y * frame->linesize[plane], 128, frame->width / 2);
        }
    }
    return frame;
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    SDL_LogSetOutputFunction(logOutput, nullptr);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "Vulkan smoke test unavailable: %s\n", SDL_GetError());
        return 77;
    }

    // X11 can present to an unmapped surface. This window never appears or
    // gains focus; the test still exercises real Vulkan buffer presentation.
    SDL_Window* window = SDL_CreateWindow("Moonshine Client startup regression test",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 320, 180,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "Vulkan smoke test unavailable: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }

    bool unavailable = false;
    {
        StartupRenderer renderer;
        DECODER_PARAMETERS params = {};
        params.window = window;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.width = 320;
        params.height = 180;
        params.frameRate = 60;
        if (!renderer.initialize(&params)) {
            std::fprintf(stderr, "Vulkan smoke test could not initialize a renderer.\n");
            unavailable = true;
        }
        else {
            renderer.prepareToRender();
            assert(initialBuffers == 1);

            VIDEO_STATS stats = {};
            {
                Pacer pacer(&renderer, &stats);
                assert(pacer.initialize(window, 60, false));
                pacer.setPaused(true);
                AVFrame* frame = av_frame_alloc();
                assert(frame);
                pacer.submitFrame(frame);
                pacer.renderOnMainThread();
                assert(renderer.videoFrames == 0);
                assert(stats.renderedFrames == 0);
                assert(initialBuffers == 1);
            }

            // Combine the real renderer and production one-frame gate. A
            // black mapping buffer must never spend the real-video budget.
            assert(renderer.presentedFrameSerial() == 0);
            VideoPauseState preview;
            preview.beginStream(true, true);
            assert(preview.synchronize().requestKeyframe);
            assert(preview.acceptFrame(true));
            assert(preview.canReceiveOutput());
            unsigned completed = 0;
            stats = {};
            {
                Pacer pacer(&renderer, &stats, &preview, [&] { completed++; });
                assert(pacer.initialize(window, 60, false));
                pacer.setPaused(true);
                renderer.waitToRender();
                for (unsigned i = 0; i < 100; ++i) {
                    pacer.submitFrame(makeVideoImage());
                }
                pacer.renderOnMainThread();
                assert(renderer.videoFrames == 1);
                assert(renderer.presentedFrameSerial() == 1);
                assert(stats.renderedFrames == 1);
                assert(completed == 1);
                assert(!preview.isPreviewPending());
                assert(!preview.canReceiveOutput());
                for (unsigned i = 0; i < 100; ++i) {
                    pacer.submitFrame(makeVideoImage());
                }
                pacer.renderOnMainThread();
                assert(renderer.videoFrames == 1);
                assert(renderer.presentedFrameSerial() == 1);
                assert(completed == 1);
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (unavailable) {
        return 77;
    }
    std::puts("Production Vulkan black mapping and exactly one real image while paused passed (no host connection).");
}
