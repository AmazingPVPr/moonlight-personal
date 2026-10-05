#pragma once

#include <atomic>
#include <cstdint>

// UI threads only change the desired state. The decoder thread observes each
// generation and owns the keyframe gate, so FFmpeg is never flushed by the UI.
class VideoPauseState
{
public:
    struct Transition {
        bool changed;
        bool paused;
        bool requestKeyframe;
    };

    bool setPaused(bool paused)
    {
        auto previous = m_Desired.load();
        for (;;) {
            if (bool(previous & 1) == paused) {
                return false;
            }
            // Preserve a generation even when a pause/resume pair is faster
            // than the decoder thread. It still discarded reference frames.
            auto next = ((previous & ~uint64_t(1)) + 2) | uint64_t(paused);
            if (m_Desired.compare_exchange_weak(previous, next)) {
                return true;
            }
        }
    }

    Transition synchronize()
    {
        auto desired = m_Desired.load();
        bool changed = desired != m_Observed;
        if (changed) {
            m_Observed = desired;
            m_AwaitingKeyframe = true;
        }
        return {changed, bool(desired & 1), changed && !(desired & 1)};
    }

    bool acceptFrame(bool keyframe)
    {
        // A transition during decode invalidates this submission as well.
        if (m_Desired.load() != m_Observed || isPaused()) {
            return false;
        }
        if (m_AwaitingKeyframe && !keyframe) {
            return false;
        }
        m_AwaitingKeyframe = false;
        return true;
    }

    bool isPaused() const { return bool(m_Desired.load() & 1); }
    bool hasTransition() const { return m_Desired.load() != m_Observed; }
    void requireKeyframe() { m_AwaitingKeyframe = true; }

private:
    std::atomic<uint64_t> m_Desired{0};
    uint64_t m_Observed = 0;
    bool m_AwaitingKeyframe = true;
};
