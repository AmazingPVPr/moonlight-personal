#pragma once

#include <algorithm>
#include <cstdint>

// Focus-delay policy used by Session's SDL loop. Unsigned elapsed arithmetic
// remains correct across SDL's 32-bit monotonic tick wrap (delay is <=1 hour).
class VideoPausePolicy
{
public:
    void setFocused(bool focused, uint32_t nowMs)
    {
        if (focused == m_Focused) {
            return;
        }
        m_Focused = focused;
        m_FocusLostAtMs = nowMs;
    }

    void setHidden(bool hidden, uint32_t nowMs)
    {
        if (hidden != m_Hidden) {
            m_Hidden = hidden;
            m_HiddenAtMs = nowMs;
        }
    }

    bool shouldPause(bool hidden, bool pauseHidden, bool pauseUnfocused,
                     int delaySeconds, uint32_t nowMs) const
    {
        return (pauseHidden && hidden) ||
               (pauseUnfocused && !m_Focused &&
                uint32_t(nowMs - m_FocusLostAtMs) >= delayMs(delaySeconds));
    }

    uint32_t waitTimeout(uint32_t maxMs, bool alreadyPaused, bool pauseUnfocused,
                         int delaySeconds, uint32_t nowMs) const
    {
        if (alreadyPaused || !pauseUnfocused || m_Focused) {
            return maxMs;
        }
        auto elapsed = uint32_t(nowMs - m_FocusLostAtMs);
        auto delay = delayMs(delaySeconds);
        return elapsed >= delay ? 0 : std::min(maxMs, delay - elapsed);
    }

    bool shouldMute(bool muteUnfocused, int unfocusedSeconds,
                    bool muteHidden, int hiddenSeconds, uint32_t nowMs) const
    {
        return (muteUnfocused && !m_Focused &&
                uint32_t(nowMs - m_FocusLostAtMs) >= delayMs(unfocusedSeconds)) ||
               (muteHidden && m_Hidden &&
                uint32_t(nowMs - m_HiddenAtMs) >= delayMs(hiddenSeconds));
    }

    uint32_t audioWaitTimeout(uint32_t maxMs, bool alreadyMuted,
                             bool muteUnfocused, int unfocusedSeconds,
                             bool muteHidden, int hiddenSeconds, uint32_t nowMs) const
    {
        if (alreadyMuted) {
            return maxMs;
        }
        auto remaining = [nowMs](uint32_t startedAt, int seconds) {
            auto delay = delayMs(seconds);
            auto elapsed = uint32_t(nowMs - startedAt);
            return elapsed >= delay ? 0U : delay - elapsed;
        };
        if (muteUnfocused && !m_Focused) {
            maxMs = std::min(maxMs, remaining(m_FocusLostAtMs, unfocusedSeconds));
        }
        if (muteHidden && m_Hidden) {
            maxMs = std::min(maxMs, remaining(m_HiddenAtMs, hiddenSeconds));
        }
        return maxMs;
    }

private:
    static uint32_t delayMs(int seconds)
    {
        return uint32_t(std::max(0, std::min(seconds, 3600))) * 1000;
    }
    bool m_Focused = true;
    uint32_t m_FocusLostAtMs = 0;
    bool m_Hidden = false;
    uint32_t m_HiddenAtMs = 0;
};
