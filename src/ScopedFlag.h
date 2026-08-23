#pragma once

#include <utility>

namespace TextureToolkit
{
    // Holds a flag for the length of a scope and puts back whatever was there, so an early return
    // cannot leave it set. A re-entrancy flag left set here silently disables texture tracking for
    // the rest of the session, and restoring the previous value (rather than clearing) keeps
    // nesting correct.
    //
    // The approach is Special K's SK_ScopedBool; the assignment is done here rather than left to
    // the caller, so the set and the restore cannot drift apart.
    class ScopedFlag
    {
    public:
        explicit ScopedFlag(bool &flag, bool value = true) noexcept
            : m_flag(flag), m_previous(std::exchange(flag, value))
        {
        }

        ~ScopedFlag() noexcept { m_flag = m_previous; }

        ScopedFlag(const ScopedFlag &) = delete;
        ScopedFlag(ScopedFlag &&) = delete;
        ScopedFlag &operator=(const ScopedFlag &) = delete;
        ScopedFlag &operator=(ScopedFlag &&) = delete;

    private:
        bool &m_flag;
        bool m_previous;
    };
}
