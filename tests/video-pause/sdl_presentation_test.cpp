#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/sdlvid.h"

#include <QCoreApplication>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

Session* Session::s_ActiveSession = nullptr;
void Session::flushWindowEvents() {}
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
void StreamUtils::scaleSourceToDestinationSurface(SDL_Rect*, SDL_Rect*) {}

// Intercept only this test process's copy operation to exercise the production
// failure branch. Other SDL operations and successful copies run normally.
static bool failCopy = false;
extern "C" int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture,
                               const SDL_Rect* src, const SDL_Rect* dst)
{
    if (failCopy) return SDL_SetError("Injected test copy failure");
    using Copy = int (*)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
    static auto realCopy = reinterpret_cast<Copy>(dlsym(RTLD_NEXT, "SDL_RenderCopy"));
    assert(realCopy);
    return realCopy(renderer, texture, src, dst);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_CRITICAL);
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    auto window = SDL_CreateWindow("SDL presentation regression test", 0, 0, 320, 180, SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return 77; }
    bool available = false;
    {
        SdlRenderer renderer;
        DECODER_PARAMETERS params = {};
        params.window = window;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.width = 320;
        params.height = 180;
        params.frameRate = 60;
        available = renderer.initialize(&params);
        if (available) {
            assert(renderer.presentedFrameSerial() == 0);
            auto frame = av_frame_alloc();
            assert(frame);
            frame->format = AV_PIX_FMT_YUV420P;
            frame->width = 320;
            frame->height = 180;
            assert(av_frame_get_buffer(frame, 32) == 0);
            for (int plane = 0; plane < 3; ++plane) {
                int height = plane == 0 ? frame->height : frame->height / 2;
                int width = plane == 0 ? frame->width : frame->width / 2;
                for (int y = 0; y < height; ++y) {
                    std::memset(frame->data[plane] + y * frame->linesize[plane], plane == 0 ? 96 : 128, width);
                }
            }
            failCopy = true;
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 0);
            failCopy = false;
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 1);
            failCopy = true;
            renderer.renderFrame(frame);
            assert(renderer.presentedFrameSerial() == 1);
            av_frame_free(&frame);
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!available) return 77;
    std::puts("Production SDL serial excludes failed video copies and counts one successful synthetic image.");
}
