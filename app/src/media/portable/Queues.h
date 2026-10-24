// xbox-cast media: small thread-safe queues (portable).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace XboxCast { namespace Media {

// Bounded FIFO. Push never blocks (producer = castcore callback thread, must return within ~1 ms): when full, the
// caller-supplied policy decides (see TryPush overflow handling in VideoDecoder / AudioOutput).
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : m_capacity(capacity) {}

    // Returns false when full (item not queued).
    bool TryPush(T&& item)
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_closed || m_q.size() >= m_capacity) return false;
            m_q.push_back(std::move(item));
        }
        m_cv.notify_one();
        return true;
    }

    // Push, evicting the oldest items when full. Returns how many were evicted.
    size_t PushEvict(T&& item)
    {
        size_t evicted = 0;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_closed) return 0;
            while (m_q.size() >= m_capacity) { m_q.pop_front(); evicted++; }
            m_q.push_back(std::move(item));
        }
        m_cv.notify_one();
        return evicted;
    }

    // Waits up to timeout. Returns false on timeout or when closed and empty.
    bool Pop(T& out, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(m_mu);
        if (!m_cv.wait_for(lk, timeout, [&] { return !m_q.empty() || m_closed; })) return false;
        if (m_q.empty()) return false;
        out = std::move(m_q.front());
        m_q.pop_front();
        return true;
    }

    bool TryPop(T& out)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_q.empty()) return false;
        out = std::move(m_q.front());
        m_q.pop_front();
        return true;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_q.clear();
    }

    template <typename Pred>
    size_t RemoveIf(Pred pred)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        size_t n = 0;
        for (auto it = m_q.begin(); it != m_q.end();) {
            if (pred(*it)) { it = m_q.erase(it); n++; } else ++it;
        }
        return n;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_q.size();
    }

    void Close()
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    void Reopen()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_closed = false;
    }

    void Wake() { m_cv.notify_all(); }

private:
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<T> m_q;
    size_t m_capacity;
    bool m_closed = false;
};

// Single-slot "latest value wins" mailbox: decode thread -> render thread. Overwriting an unconsumed value counts
// as a dropped (never presented) frame.
template <typename T>
class LatestMailbox {
public:
    // Returns true if an unconsumed value was replaced.
    bool Put(T&& v)
    {
        bool replaced;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            replaced = m_has;
            m_v = std::move(v);
            m_has = true;
        }
        m_cv.notify_one();
        return replaced;
    }

    bool Take(T& out)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_has) return false;
        out = std::move(m_v);
        m_v = T();
        m_has = false;
        return true;
    }

    // Wait until a value is available (or timeout / Wake()).
    bool Wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait_for(lk, timeout, [&] { return m_has || m_wake; });
        m_wake = false;
        return m_has;
    }

    void Wake()
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_wake = true;
        }
        m_cv.notify_all();
    }

    bool Has() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_has;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_v = T();
        m_has = false;
    }

private:
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    T m_v{};
    bool m_has = false;
    bool m_wake = false;
};

}} // namespace XboxCast::Media
