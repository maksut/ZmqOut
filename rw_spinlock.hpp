#pragma once

#include <atomic>
#include <immintrin.h>

// shamelessly stolen from: nova-tt/rw_spinlock.hpp
class rw_spinlock
{
    static const uint32_t unlocked_state = 0;
    static const uint32_t locked_state = 0x80000000;
    static const uint32_t reader_mask = 0x7fffffff;

#if __cplusplus >= 201103L
    rw_spinlock(rw_spinlock const& rhs) = delete;
    rw_spinlock& operator=(rw_spinlock const& rhs) = delete;
#else
    rw_spinlock(rw_spinlock const& rhs);
    rw_spinlock& operator=(rw_spinlock const& rhs);
#endif

public:
    struct scoped_lock
    {
        scoped_lock(rw_spinlock& sl) :
            sl_(sl)
        {
            sl_.lock();
        }

        ~scoped_lock(void)
        {
            sl_.unlock();
        }

        rw_spinlock& sl_;
    };

    typedef scoped_lock unique_lock;

    struct shared_lock
    {
        shared_lock(rw_spinlock& sl) :
            sl_(sl)
        {
            sl_.lock_shared();
        }

        ~shared_lock(void)
        {
            sl_.unlock_shared();
        }

        rw_spinlock& sl_;
    };

    rw_spinlock(void) :
        state(uint32_t(unlocked_state))
    {}

    ~rw_spinlock(void)
    {
        assert(state == unlocked_state);
    }

    void lock(void)
    {
        for (;;) {
            while (state.load(std::memory_order_relaxed) != unlocked_state)
                _mm_pause();

            uint32_t expected = unlocked_state;
            if (state.compare_exchange_weak(expected, locked_state, std::memory_order_acquire))
                break;
        }
    }

    bool try_lock(void)
    {
        uint32_t expected = unlocked_state;
        if (state.compare_exchange_strong(expected, locked_state, std::memory_order_acquire))
            return true;
        else
            return false;
    }

    void unlock(void)
    {
        assert(state.load(std::memory_order_relaxed) == locked_state);
        state.store(unlocked_state, std::memory_order_release);
    }

    void lock_shared(void)
    {
        for (;;) {
            /* with the mask, the cas will fail, locked exclusively */
            uint32_t current_state = state.load(std::memory_order_acquire) & reader_mask;
            const uint32_t next_state = current_state + 1;

            if (state.compare_exchange_weak(current_state, next_state, std::memory_order_acquire))
                break;
            _mm_pause();
        }
    }

    bool try_lock_shared(void)
    {
        /* with the mask, the cas will fail, locked exclusively */
        uint32_t current_state = state.load(std::memory_order_acquire) & reader_mask;
        const uint32_t next_state = current_state + 1;

        if (state.compare_exchange_strong(current_state, next_state, std::memory_order_acquire))
            return true;
        else
            return false;
    }

    void unlock_shared(void)
    {
        for (;;) {
            uint32_t current_state = state.load(std::memory_order_relaxed); /* we don't need the reader_mask */
            const uint32_t next_state = current_state - 1;

            if (state.compare_exchange_weak(current_state, uint32_t(next_state)))
                break;
            _mm_pause();
        }
    }

private:
    std::atomic<uint32_t> state;
};
