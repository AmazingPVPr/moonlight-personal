#include "streaming/video/pausestate.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"
#include "streaming/streamutils.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

// Pacer only needs the display rate and clock, so a real display/host is not
// required. Its production queues, AVFrame ownership, and render thread run.
int StreamUtils::getDisplayRefreshRate(SDL_Window*) { return 60; }
extern "C" uint64_t LiGetMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class CountingRenderer : public IFFmpegRenderer
{
public:
    explicit CountingRenderer(bool threaded) : IFFmpegRenderer(RendererType::Unknown), threaded(threaded) {}
    bool initialize(PDECODER_PARAMETERS) override { return true; }
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return true; }
    void notifyOverlayUpdated(Overlay::OverlayType) override {}
    bool isRenderThreadSupported() override { return threaded; }
    void renderFrame(AVFrame*) override
    {
        std::lock_guard<std::mutex> guard(lock);
        rendered++;
        changed.notify_all();
    }
    bool waitForRender(int count)
    {
        std::unique_lock<std::mutex> guard(lock);
        return changed.wait_for(guard, std::chrono::seconds(3), [&] { return rendered >= count; });
    }
    std::atomic<int> rendered{0};
private:
    bool threaded;
    std::mutex lock;
    std::condition_variable changed;
};

static AVFrame* makeFrame(std::atomic<int>& released)
{
    auto frame = av_frame_alloc();
    assert(frame);
    frame->buf[0] = av_buffer_create(static_cast<uint8_t*>(av_malloc(16)), 16,
        [](void* opaque, uint8_t* data) {
            static_cast<std::atomic<int>*>(opaque)->fetch_add(1);
            av_free(data);
        }, &released, 0);
    assert(frame->buf[0]);
    frame->pkt_dts = LiGetMicroseconds();
    return frame;
}

static void testKeyframeRecovery()
{
    VideoPauseState state;
    assert(!state.acceptFrame(false));
    assert(state.acceptFrame(true));
    assert(state.acceptFrame(false));

    assert(state.setPaused(true));
    // Immediately gate submissions even before decoder-thread synchronization.
    assert(!state.acceptFrame(true));
    auto pause = state.synchronize();
    assert(pause.changed && pause.paused && !pause.requestKeyframe);
    assert(!state.acceptFrame(false));
    assert(!state.acceptFrame(true));
    assert(!state.setPaused(true));
    assert(!state.synchronize().changed);

    assert(state.setPaused(false));
    auto resume = state.synchronize();
    assert(resume.changed && !resume.paused && resume.requestKeyframe);
    // Reference-dependent frames must not reach FFmpeg after missed input.
    for (int i = 0; i < 1000; i++) {
        assert(!state.acceptFrame(false));
        assert(!state.synchronize().requestKeyframe);
    }
    assert(state.acceptFrame(true));
    assert(state.acceptFrame(false));

    // A rapid desktop switch away and back still invalidates references.
    state.setPaused(true);
    state.setPaused(false);
    assert(!state.acceptFrame(true));
    assert(state.synchronize().requestKeyframe);
    assert(!state.acceptFrame(false));
    assert(state.acceptFrame(true));
    state.requireKeyframe();
    assert(!state.acceptFrame(false));
    assert(state.acceptFrame(true));
}

static void testQueuedSurfacesAndResume(bool threaded)
{
    std::atomic<int> released{0};
    CountingRenderer renderer(threaded);
    VIDEO_STATS stats = {};
    {
        Pacer pacer(&renderer, &stats);
        assert(pacer.initialize(nullptr, 60, false));
        pacer.setPaused(true);
        for (int i = 0; i < 1000; i++) {
            pacer.submitFrame(makeFrame(released));
        }
        pacer.renderOnMainThread();
        assert(renderer.rendered == 0);
        assert(released == 1000);

        pacer.setPaused(false);
        pacer.submitFrame(makeFrame(released));
        if (threaded) {
            assert(renderer.waitForRender(1));
        }
        else {
            pacer.renderOnMainThread();
        }
        assert(renderer.rendered == 1);
    }
    assert(released == 1001);
}

static void testPendingMainThreadFramesAreReleased()
{
    std::atomic<int> released{0};
    CountingRenderer renderer(false);
    VIDEO_STATS stats = {};
    {
        Pacer pacer(&renderer, &stats);
        assert(pacer.initialize(nullptr, 60, false));
        for (int i = 0; i < 3; i++) {
            pacer.submitFrame(makeFrame(released));
        }
        pacer.setPaused(true);
        pacer.renderOnMainThread(); // stale SDL frame-ready events are harmless
        assert(renderer.rendered == 0);
        assert(released == 3);
        pacer.setPaused(false);
        pacer.renderOnMainThread();
        assert(renderer.rendered == 0);
        pacer.submitFrame(makeFrame(released));
        pacer.renderOnMainThread();
        assert(renderer.rendered == 1);
    }
    assert(released == 4);
}

static void testPausedThreadTeardown()
{
    // Race startup/empty-queue waits against shutdown. A lost notification
    // would hang destruction instead of returning to the next iteration.
    for (int i = 0; i < 100; i++) {
        std::atomic<int> released{0};
        CountingRenderer renderer(true);
        VIDEO_STATS stats = {};
        {
            Pacer pacer(&renderer, &stats);
            assert(pacer.initialize(nullptr, 60, false));
            pacer.setPaused(true);
            pacer.submitFrame(makeFrame(released));
            pacer.setPaused(false);
            pacer.setPaused(true);
        }
        assert(renderer.rendered == 0);
        assert(released == 1);
    }
}

int main()
{
    assert(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER) == 0);
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_ERROR);
    testKeyframeRecovery();
    testPendingMainThreadFramesAreReleased();
    testQueuedSurfacesAndResume(false);
    testQueuedSurfacesAndResume(true);
    testPausedThreadTeardown();
    SDL_Quit();
    std::puts("Video keyframe recovery and production pacer pause/resume tests passed.");
}
