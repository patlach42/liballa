/*
 * Copyright (C) 2026 patlach42
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace monotrypt::usb {

// One bounded wake channel: an eventfd plus the number of threads currently
// registered to block on it.
//
// A producer publishes its state change and then calls `signalIfWaiting()`,
// which skips the eventfd write when no thread can be woken. That removes a
// syscall from every USB completion in the common case where the consumer is
// still busy rather than blocked.
//
// Lost-wakeup argument. `Registration` increments the waiter count with a
// seq_cst read-modify-write before the caller's first state check, and
// `signalIfWaiting()` executes a seq_cst fence after the caller's publication
// and before reading the count. Both participate in the single total order of
// seq_cst operations, so for any producer/waiter pair at least one side
// observes the other:
//
//   - if the waiter's increment precedes the producer's fence, the producer
//     reads a nonzero count and signals;
//   - otherwise the producer's publication precedes the waiter's first state
//     check, and the waiter observes the new state and never blocks.
//
// A signal that lands before `poll()` is not lost either: eventfd keeps a
// counter, so `poll()` returns immediately on an already-signalled channel.
//
// Terminal transitions (stop, transport failure) use `signal()` instead, which
// writes unconditionally; correctness there must not depend on the gate.
class WakeChannel {
public:
    WakeChannel() = default;
    ~WakeChannel() { close(); }

    WakeChannel(const WakeChannel&) = delete;
    WakeChannel& operator=(const WakeChannel&) = delete;

    bool open() noexcept {
        if (fd_ >= 0) return true;
        fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        return fd_ >= 0;
    }

    void close() noexcept {
        if (fd_ < 0) return;
        ::close(fd_);
        fd_ = -1;
    }

    bool valid() const noexcept { return fd_ >= 0; }

    // Unconditional wake. Use for terminal transitions.
    void signal() const noexcept {
        if (fd_ < 0) return;
        const eventfd_t one = 1;
        (void)::eventfd_write(fd_, one);
    }

    // Gated wake. The caller must already have published its state change.
    void signalIfWaiting() const noexcept {
        if (fd_ < 0) return;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters_.load(std::memory_order_seq_cst) > 0) signal();
    }

    int32_t waiterCount() const noexcept {
        return waiters_.load(std::memory_order_acquire);
    }

    // Scoped waiter registration. Construct before the first state check and
    // keep alive until after the last one.
    class Registration {
    public:
        explicit Registration(const WakeChannel& channel) noexcept
            : channel_(channel) {
            channel_.waiters_.fetch_add(1, std::memory_order_seq_cst);
        }
        ~Registration() {
            channel_.waiters_.fetch_sub(1, std::memory_order_seq_cst);
        }
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

    private:
        const WakeChannel& channel_;
    };

    bool poll(int timeoutMs) const noexcept {
        if (fd_ < 0) return false;
        pollfd descriptor{fd_, POLLIN, 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, timeoutMs);
        } while (result < 0 && errno == EINTR);
        return result > 0;
    }

    bool pollUntil(std::chrono::steady_clock::time_point deadline) const noexcept {
        if (fd_ < 0) return false;
        pollfd descriptor{fd_, POLLIN, 0};
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            timespec timeout{
                static_cast<time_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(remaining).count()),
                static_cast<long>((remaining % std::chrono::seconds(1)).count())
            };
            const int result = ::ppoll(&descriptor, 1, &timeout, nullptr);
            if (result > 0) return true;
            if (result == 0) return false;
            if (errno != EINTR) return false;
        }
    }

    void drain() const noexcept {
        if (fd_ < 0) return;
        eventfd_t value = 0;
        (void)::eventfd_read(fd_, &value);
    }

private:
    int fd_ = -1;
    mutable std::atomic<int32_t> waiters_{0};
};

} // namespace monotrypt::usb
