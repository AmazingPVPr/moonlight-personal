#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// UI/presentation threads update atomic desired state. Only the decoder thread
// owns keyframe admission and observes epochs before touching FFmpeg. Timer
// flags/deadlines share the same atomic word, so a stale render cannot restart
// a newer preview after a focus/visibility transition.
class VideoPauseState
{
public:
    static constexpr uint32_t StartupWarmupMs = 1000;
    static constexpr uint32_t PreviewFailureTimeoutMs = 2000;
    enum class PreviewChange { Unchanged, Completed, Blocked };

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
    }

    bool setPaused(bool paused)
    {
        auto previous = m_Desired.load();
        for (;;) {
            if (bool(previous & Paused) == paused) {
                return false;
            }
            auto flags = (previous & (Preview | Warmup | Attempt)) | (paused ? Paused : 0);
            auto next = withDeadline(nextEpoch(previous) | flags, deadline(previous));
            if (m_Desired.compare_exchange_weak(previous, next)) {
                return true;
            }
        }
    }

    Transition synchronize()
    {
        auto desired = m_Desired.load();
        // Starting/completing a timer while running need not invalidate codec
        // references. A paused completion advances the epoch and flushes them.
        bool changed = controlState(desired) != controlState(m_Observed);
        m_Observed = desired;
        if (changed) {
            requireKeyframe();
        }
        return {changed, bool(desired & Paused),
                changed && (!(desired & Paused) ||
                            ((desired & Preview) && !(desired & PreviewBlocked)))};
    }

    bool acceptFrame(bool keyframe, uint32_t nowMs = 0)
    {
        auto desired = m_Desired.load();
        if (hasTransition() || ((desired & Paused) && !previewAllowed(desired, nowMs))) {
            return false;
        }
        if (m_AwaitingKeyframe && !keyframe) {
            return false;
        }
        m_AwaitingKeyframe = false;

        // Bound admission before the first successful video presentation.
        // Delayed codecs may need several packets; admit their P-frames after
        // the IDR instead of wedging them behind a single-input limit.
        while ((desired & (Preview | PreviewBlocked | Warmup | Attempt)) == Preview) {
            auto next = withDeadline(desired | Attempt, nowMs + PreviewFailureTimeoutMs);
            if (m_Desired.compare_exchange_weak(desired, next)) {
                break;
            }
        }
        return true;
    }

    bool canReceiveOutput(uint32_t nowMs = 0) const
    {
        auto desired = m_Desired.load();
        return !(desired & Paused) || previewAllowed(desired, nowMs);
    }

    // Called after the renderer confirms an actual video image was submitted.
    // A generation captured before rendering prevents a stale in-flight frame
    // from starting the timer after a new pause/refresh epoch.
    bool notePresented(uint32_t nowMs, uint32_t generation)
    {
        auto previous = m_Desired.load();
        while (controlState(previous) == generation &&
               (previous & (Preview | PreviewBlocked | Warmup)) == Preview &&
               (!(previous & Attempt) || !expired(previous, nowMs))) {
            auto next = withDeadline((previous & ~Attempt) | Warmup, nowMs + StartupWarmupMs);
            if (m_Desired.compare_exchange_weak(previous, next)) {
                return true;
            }
        }
        return false;
    }

    PreviewChange advancePreview(uint32_t nowMs)
    {
        auto previous = m_Desired.load();
        for (;;) {
            if (!(previous & Preview) || (previous & PreviewBlocked) ||
                    !(previous & (Warmup | Attempt)) || !expired(previous, nowMs)) {
                return PreviewChange::Unchanged;
            }
            bool blocked = !(previous & Warmup) && (previous & Paused);
            uint64_t next = previous & ~(uint64_t(Flags) | DeadlineMask);
            if (previous & Paused) {
                next = nextEpoch(previous) | Paused;
            }
            if (blocked) {
                next |= Preview | PreviewBlocked;
            }
            if (m_Desired.compare_exchange_weak(previous, next)) {
                return blocked ? PreviewChange::Blocked : PreviewChange::Completed;
            }
        }
    }

    bool blockFailedPreview()
    {
        auto previous = m_Desired.load();
        while ((previous & (Paused | Preview)) == (Paused | Preview) && !(previous & PreviewBlocked)) {
            if (m_Desired.compare_exchange_weak(previous,
                    nextEpoch(previous) | Paused | Preview | PreviewBlocked)) {
                return true;
            }
        }
        return false;
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

    uint32_t previewWaitTimeout(uint32_t maxMs, uint32_t nowMs) const
    {
        auto desired = m_Desired.load();
        if (!(desired & Preview) || (desired & PreviewBlocked) || !(desired & (Warmup | Attempt))) {
            return maxMs;
        }
        return expired(desired, nowMs) ? 0 : std::min(maxMs, uint32_t(deadline(desired) - nowMs));
    }

    bool isPaused() const { return bool(m_Desired.load() & Paused); }
    bool isPreviewPending() const { return bool(m_Desired.load() & Preview); }
    bool canPresentPreview(uint32_t nowMs = 0) const { return previewAllowed(m_Desired.load(), nowMs); }
    uint32_t previewGeneration() const { return controlState(m_Desired.load()); }
    bool hasTransition() const { return controlState(m_Desired.load()) != controlState(m_Observed); }
    void requireKeyframe() { m_AwaitingKeyframe = true; }

private:
    // Low 32 bits hold flags/epoch; high 32 bits hold an absolute monotonic
    // millisecond deadline. Armed flags make deadline zero valid after wrap.
    static constexpr uint32_t Paused = 1;
    static constexpr uint32_t Preview = 2;
    static constexpr uint32_t PreviewBlocked = 4;
    static constexpr uint32_t Warmup = 8;
    static constexpr uint32_t Attempt = 16;
    static constexpr uint32_t Flags = Paused | Preview | PreviewBlocked | Warmup | Attempt;
    static constexpr uint64_t DeadlineMask = uint64_t(UINT32_MAX) << 32;

    static uint32_t deadline(uint64_t state) { return uint32_t(state >> 32); }
    static uint64_t withDeadline(uint64_t state, uint32_t deadlineMs)
    {
        return uint32_t(state) | (uint64_t(deadlineMs) << 32);
    }
    static uint64_t nextEpoch(uint64_t state) { return uint32_t((uint32_t(state) & ~Flags) + 32); }
    static uint32_t controlState(uint64_t state) { return uint32_t(state) & ~(Preview | Warmup | Attempt); }
    static bool expired(uint64_t state, uint32_t nowMs)
    {
        return int32_t(nowMs - deadline(state)) >= 0;
    }
    static bool previewAllowed(uint64_t state, uint32_t nowMs)
    {
        return (state & (Preview | PreviewBlocked)) == Preview &&
               (!(state & (Warmup | Attempt)) || !expired(state, nowMs));
    }

    std::atomic<uint64_t> m_Desired{0};
    uint64_t m_Observed = 0;
    bool m_AwaitingKeyframe = true;
};
