#pragma once

#include <utility>

namespace TextureToolkit
{
    // Holds a flag for the length of a scope and puts back whatever was there, so an early return
    // cannot leave it set -- and a flag left set here silently disables texture tracking for the
    // rest of the session.
    //
    // Special K's SK_ScopedBool does the same, except it leaves the assignment to the caller and so
    // still reads as two statements that can drift apart; the value is set here instead. Copying
    // would restore twice, the second time over someone else's value.
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
