#pragma once

// Coalesces authored particle-system notifications while device recovery keeps
// normal Engine work closed. One repeated track stays specific; different
// tracks, or any structural notification, widen to the existing -1
// "everything changed" broadcast.
namespace particlesystemchange {

class DeferredReplay
{
public:
    bool DeferIfBlocked(bool blocked, int track)
    {
        if (!blocked) return false;
        Queue(track);
        return true;
    }

    void Queue(int track)
    {
        const int normalizedTrack = track < 0 ? -1 : track;
        if (!m_pending)
        {
            m_pending = true;
            m_track   = normalizedTrack;
        }
        else if (m_track < 0 || normalizedTrack < 0 ||
                 m_track != normalizedTrack)
        {
            m_track = -1;
        }
    }

    bool Take(int& track)
    {
        if (!m_pending) return false;
        track = m_track;
        Reset();
        return true;
    }

    bool Pending() const
    {
        return m_pending;
    }

    template <typename TApply, typename TBlocked>
    bool Replay(TApply apply, TBlocked blocked)
    {
        int track = -1;
        if (!Take(track)) return true;

        try
        {
            apply(track);
        }
        catch (...)
        {
            Queue(track);
            return false;
        }

        if (blocked())
        {
            // A reentrant blocked notification may already be pending. Fold
            // the original request back into that batch before skipping.
            Queue(track);
            return false;
        }
        return true;
    }

    void Reset()
    {
        m_pending = false;
        m_track   = -1;
    }

private:
    bool m_pending = false;
    int  m_track   = -1;
};

} // namespace particlesystemchange
