#pragma once

#include <atomic>
#include <cstdint>

// UI/presentation threads update atomic desired flags. Only the decoder thread
// owns keyframe/input admission and observes epochs before touching FFmpeg.
class VideoPauseState
{
public:
    struct Transition {
        bool changed;
        bool paused;
        bool requestKeyframe;
    };

    void beginStream(bool paused, bool preview)
    {
        m_Desired.store((paused ? Paused : 0) | (preview ? Preview : 0));
        m_Observed = 0;
        m_AwaitingKeyframe = true;
        m_PreviewInputSubmitted = false;
    }

    bool setPaused(bool paused)
    {
        auto previous = m_Desired.load();
        for (;;) {
            if (bool(previous & Paused) == paused) {
                return false;
            }
            auto flags = (previous & Preview) | (paused ? Paused : 0);
            if (m_Desired.compare_exchange_weak(previous, nextEpoch(previous) | flags)) {
                return true;
            }
        }
    }

    Transition synchronize()
    {
        auto desired = m_Desired.load();
        // Completing a preview while already running need not invalidate
        // references. A paused completion advances the epoch and flushes them.
        bool changed = (desired & ~Preview) != (m_Observed & ~Preview);
        m_Observed = desired;
        if (changed) {
            requireKeyframe();
        }
        return {changed, bool(desired & Paused),
                changed && (!(desired & Paused) ||
                            ((desired & Preview) && !(desired & PreviewBlocked)))};
    }

    bool acceptFrame(bool keyframe)
    {
        auto desired = m_Desired.load();
        if (hasTransition()) {
            return false;
        }
        if (desired & Paused) {
            if (!(desired & Preview) || (desired & PreviewBlocked) || m_PreviewInputSubmitted) {
                return false;
            }
            if (!keyframe) {
                return false;
            }
            m_PreviewInputSubmitted = true;
        }
        if (m_AwaitingKeyframe && !keyframe) {
            return false;
        }
        m_AwaitingKeyframe = false;
        return true;
    }

    bool canReceiveOutput() const
    {
        return !isPaused() || (canPresentPreview() && m_PreviewInputSubmitted);
    }

    bool finishPreview()
    {
        auto previous = m_Desired.load();
        for (;;) {
            if (!(previous & Preview)) {
                return false;
            }
            auto next = previous & ~(Preview | PreviewBlocked);
            if (previous & Paused) {
                next = nextEpoch(previous) | Paused;
            }
            if (m_Desired.compare_exchange_weak(previous, next)) {
                return true;
            }
        }
    }

    void blockFailedPreview()
    {
        auto previous = m_Desired.load();
        while ((previous & (Paused | Preview)) == (Paused | Preview) && !(previous & PreviewBlocked)) {
            if (m_Desired.compare_exchange_weak(previous,
                    nextEpoch(previous) | Paused | Preview | PreviewBlocked)) {
                return;
            }
        }
    }

    bool refreshPreview()
    {
        auto previous = m_Desired.load();
        while ((previous & (Preview | PreviewBlocked)) == (Preview | PreviewBlocked)) {
            if (m_Desired.compare_exchange_weak(previous,
                    nextEpoch(previous) | (previous & (Paused | Preview)))) {
                return true;
            }
        }
        return false;
    }

    bool isPaused() const { return bool(m_Desired.load() & Paused); }
    bool isPreviewPending() const { return bool(m_Desired.load() & Preview); }
    bool canPresentPreview() const
    {
        auto desired = m_Desired.load();
        return (desired & (Preview | PreviewBlocked)) == Preview;
    }
    bool hasTransition() const { return (m_Desired.load() & ~Preview) != (m_Observed & ~Preview); }
    void requireKeyframe() { m_AwaitingKeyframe = true; m_PreviewInputSubmitted = false; }

private:
    static constexpr uint64_t Paused = 1;
    static constexpr uint64_t Preview = 2;
    static constexpr uint64_t PreviewBlocked = 4;
    static constexpr uint64_t Flags = Paused | Preview | PreviewBlocked;
    static uint64_t nextEpoch(uint64_t state) { return (state & ~Flags) + 8; }

    std::atomic<uint64_t> m_Desired{0};
    uint64_t m_Observed = 0;
    bool m_AwaitingKeyframe = true;
    bool m_PreviewInputSubmitted = false;
};
