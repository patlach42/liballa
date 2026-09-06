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

#include <gtest/gtest.h>

#include <liblowlatencyaudio/WakeChannel.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using monotrypt::usb::WakeChannel;

TEST(WakeChannel, GatedSignalIsSkippedWithoutARegisteredWaiter) {
    WakeChannel channel;
    ASSERT_TRUE(channel.open());
    ASSERT_EQ(channel.waiterCount(), 0);

    channel.signalIfWaiting();
    EXPECT_FALSE(channel.poll(0)) << "gate must skip the write when nobody waits";

    // The unconditional path is what terminal transitions rely on.
    channel.signal();
    EXPECT_TRUE(channel.poll(0));
    channel.drain();
    EXPECT_FALSE(channel.poll(0)) << "a single drain must clear the counter";
}

TEST(WakeChannel, GatedSignalWritesWhileAWaiterIsRegistered) {
    WakeChannel channel;
    ASSERT_TRUE(channel.open());
    {
        const WakeChannel::Registration outer(channel);
        EXPECT_EQ(channel.waiterCount(), 1);
        {
            const WakeChannel::Registration inner(channel);
            EXPECT_EQ(channel.waiterCount(), 2);
        }
        EXPECT_EQ(channel.waiterCount(), 1);
        channel.signalIfWaiting();
        EXPECT_TRUE(channel.poll(0));
        channel.drain();
    }
    EXPECT_EQ(channel.waiterCount(), 0);
}

// The gate is only sound if a producer that observes no waiter cannot have
// raced a consumer that is about to block. Hammer exactly that window: the
// consumer registers and then re-reads the published state, while the producer
// publishes and then reads the waiter count.
TEST(WakeChannel, GatedSignalNeverLosesAWakeup) {
    WakeChannel channel;
    ASSERT_TRUE(channel.open());

    constexpr uint64_t kRounds = 20000;
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> lostWakeups{0};
    std::atomic<uint64_t> blockedRounds{0};
    std::atomic<uint64_t> consumerRound{0};

    std::thread consumer([&] {
        for (uint64_t round = 1; round <= kRounds; ++round) {
            consumerRound.store(round, std::memory_order_release);
            const WakeChannel::Registration waiting(channel);
            // Generous deadline: a lost wakeup shows up as a timeout taken
            // while the state has in fact already advanced.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool blocked = false;
            while (published.load(std::memory_order_acquire) < round) {
                blocked = true;
                if (!channel.pollUntil(deadline)) {
                    lostWakeups.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                channel.drain();
            }
            if (blocked) blockedRounds.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (uint64_t round = 1; round <= kRounds; ++round) {
        // Let the consumer reach this round so publication lands inside its
        // register-then-check window rather than long before it.
        while (consumerRound.load(std::memory_order_acquire) < round) {
            std::this_thread::yield();
        }
        for (uint64_t spin = 0; spin < (round % 7); ++spin) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        published.store(round, std::memory_order_release);
        channel.signalIfWaiting();
    }
    consumer.join();

    EXPECT_EQ(lostWakeups.load(), 0u);
    // Guard against the test degenerating into "producer always won the race",
    // which would exercise none of the blocking path.
    EXPECT_GT(blockedRounds.load(), 0u)
        << "no round ever blocked; the race window was not exercised";
}

} // namespace
