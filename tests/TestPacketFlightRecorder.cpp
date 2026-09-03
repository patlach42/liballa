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

#include <liblowlatencyaudio/PacketFlightRecorder.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using monotrypt::usb::PacketFlightRecorder;
using Event = PacketFlightRecorder::Event;

TEST(PacketFlightRecorder, DisabledByDefaultSoShippingBuildsPayNothing) {
    PacketFlightRecorder recorder(64);
    EXPECT_FALSE(recorder.enabled());
    recorder.record(Event::QuantumRefused, 1, 2, 3, 4, 5);
    EXPECT_EQ(recorder.recorded(), 0u);

    PacketFlightRecorder::Record out[8]{};
    EXPECT_EQ(recorder.snapshot(out, 8), 0u);
}

TEST(PacketFlightRecorder, RoundsCapacityUpToAPowerOfTwo) {
    EXPECT_EQ(PacketFlightRecorder(1).capacity(), 1u);
    EXPECT_EQ(PacketFlightRecorder(3).capacity(), 4u);
    EXPECT_EQ(PacketFlightRecorder(64).capacity(), 64u);
    EXPECT_EQ(PacketFlightRecorder(100).capacity(), 128u);
}

TEST(PacketFlightRecorder, PreservesEveryFieldOfARecordedEvent) {
    PacketFlightRecorder recorder(8);
    recorder.setEnabled(true);
    recorder.record(Event::QuantumRefused, 1234, 60, 64, 130, 96);

    PacketFlightRecorder::Record out[8]{};
    ASSERT_EQ(recorder.snapshot(out, 8), 1u);
    EXPECT_EQ(out[0].event, Event::QuantumRefused);
    EXPECT_EQ(out[0].timestampNs, 1234u);
    EXPECT_EQ(out[0].a, 60u);
    EXPECT_EQ(out[0].b, 64u);
    EXPECT_EQ(out[0].ringFrames, 130u);
    EXPECT_EQ(out[0].queuedFrames, 96u);
    EXPECT_NE(out[0].sequence, 0u) << "a populated slot must carry a sequence";
}

// The recorder must degrade by losing history, never by blocking a realtime
// producer, so a full buffer keeps the newest events and counts the loss.
TEST(PacketFlightRecorder, WrapKeepsNewestEventsAndCountsTheLoss) {
    PacketFlightRecorder recorder(4);
    recorder.setEnabled(true);
    for (uint32_t i = 0; i < 10; ++i)
        recorder.record(Event::PlaybackComplete, i, i, 0, 0, 0);

    EXPECT_EQ(recorder.recorded(), 10u);
    EXPECT_EQ(recorder.dropped(), 6u);

    PacketFlightRecorder::Record out[4]{};
    ASSERT_EQ(recorder.snapshot(out, 4), 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_EQ(out[i].a, 6u + i) << "slot " << i << " is not the newest run";
        EXPECT_EQ(out[i].timestampNs, 6u + i);
    }
}

TEST(PacketFlightRecorder, SnapshotSmallerThanHistoryReturnsTheTail) {
    PacketFlightRecorder recorder(8);
    recorder.setEnabled(true);
    for (uint32_t i = 0; i < 8; ++i)
        recorder.record(Event::CaptureComplete, i, i, 0, 0, 0);

    PacketFlightRecorder::Record out[3]{};
    ASSERT_EQ(recorder.snapshot(out, 3), 3u);
    EXPECT_EQ(out[0].a, 5u);
    EXPECT_EQ(out[1].a, 6u);
    EXPECT_EQ(out[2].a, 7u);
}

TEST(PacketFlightRecorder, EnablingClearsHistoryFromAPreviousSession) {
    PacketFlightRecorder recorder(8);
    recorder.setEnabled(true);
    recorder.record(Event::PlaybackUnderrun, 1, 1, 1, 1, 1);
    ASSERT_EQ(recorder.recorded(), 1u);

    recorder.setEnabled(false);
    recorder.setEnabled(true);
    EXPECT_EQ(recorder.recorded(), 0u);

    PacketFlightRecorder::Record out[8]{};
    EXPECT_EQ(recorder.snapshot(out, 8), 0u);
}

// Both the USB completion path and the render thread record, so slot claiming
// must not lose or alias events under concurrency.
TEST(PacketFlightRecorder, ConcurrentProducersClaimDistinctSlots) {
    constexpr int kProducers = 4;
    constexpr uint32_t kPerProducer = 5000;
    PacketFlightRecorder recorder(kProducers * kPerProducer);
    recorder.setEnabled(true);

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&recorder, p] {
            for (uint32_t i = 0; i < kPerProducer; ++i) {
                recorder.record(Event::QuantumOffered, i,
                                static_cast<uint32_t>(p), i, 0, 0);
            }
        });
    }
    for (auto& producer : producers) producer.join();

    const uint64_t expected = kProducers * kPerProducer;
    ASSERT_EQ(recorder.recorded(), expected);
    EXPECT_EQ(recorder.dropped(), 0u);

    std::vector<PacketFlightRecorder::Record> out(recorder.capacity());
    const size_t count = recorder.snapshot(out.data(), out.size());
    ASSERT_EQ(count, expected);

    // Every claimed slot must have been written exactly once, and each
    // producer's events must all be present.
    std::vector<int> perProducer(kProducers, 0);
    for (size_t i = 0; i < count; ++i) {
        ASSERT_NE(out[i].sequence, 0u) << "slot " << i << " was never written";
        ASSERT_LT(out[i].a, static_cast<uint32_t>(kProducers));
        ++perProducer[out[i].a];
    }
    for (int p = 0; p < kProducers; ++p)
        EXPECT_EQ(perProducer[p], static_cast<int>(kPerProducer));
}

// The realtime paths call record() unconditionally, so a disabled recorder is
// on the hot path of every USB completion and every render quantum. It must
// cost an atomic load and nothing else - in particular it must not touch the
// storage, which would pull a cold line into cache on the audio thread.
TEST(PacketFlightRecorder, DisabledRecordingLeavesStorageUntouched) {
    PacketFlightRecorder recorder(8);
    recorder.setEnabled(true);
    recorder.record(Event::PlaybackComplete, 11, 22, 33, 44, 55);
    recorder.setEnabled(false);

    for (int i = 0; i < 1000; ++i)
        recorder.record(Event::QuantumRefused, 99, 99, 99, 99, 99);

    EXPECT_EQ(recorder.recorded(), 1u) << "disabled recording must not claim slots";
    PacketFlightRecorder::Record out[8]{};
    ASSERT_EQ(recorder.snapshot(out, 8), 1u);
    EXPECT_EQ(out[0].timestampNs, 11u) << "the retained record was overwritten";
    EXPECT_EQ(out[0].a, 22u);
}

// Records carry a shared timeline so events from the USB completion path and
// the render thread can be interleaved during analysis.
TEST(PacketFlightRecorder, MonotonicClockIsNonDecreasing) {
    const uint64_t first = monotrypt::usb::monotonicNowNs();
    const uint64_t second = monotrypt::usb::monotonicNowNs();
    EXPECT_GT(first, 0u);
    EXPECT_GE(second, first);
}

// The instrument exists to explain rare events, and a rare event is exactly
// what a keep-the-newest policy throws away: the first device run offered
// 143329 events into a 4096 slot buffer, so the single refusal worth
// explaining was evicted by the steady-state traffic that followed it.
TEST(PacketFlightRecorder, FreezeTriggerPreservesTheRunUpToTheEvent) {
    PacketFlightRecorder recorder(8);
    recorder.setFreezeTrigger(Event::QuantumRefused);
    recorder.setEnabled(true);

    // Context leading up to the incident.
    for (uint32_t i = 0; i < 5; ++i)
        recorder.record(Event::QuantumOffered, i, i, 0, 100 + i, 0);
    recorder.record(Event::QuantumRefused, 99, 60, 64, 130, 96);
    EXPECT_TRUE(recorder.frozen());

    // Steady-state traffic that would otherwise evict the incident.
    for (uint32_t i = 0; i < 100; ++i)
        recorder.record(Event::PlaybackComplete, 500 + i, i, 0, 50, 0);

    PacketFlightRecorder::Record out[8]{};
    const size_t count = recorder.snapshot(out, 8);
    ASSERT_EQ(count, 6u) << "post-trigger traffic must not be recorded";
    EXPECT_EQ(out[5].event, Event::QuantumRefused)
        << "the trigger must remain the newest record";
    EXPECT_EQ(out[5].ringFrames, 130u);
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(out[i].event, Event::QuantumOffered)
            << "context record " << i << " was lost";
        EXPECT_EQ(out[i].ringFrames, 100u + i);
    }
}

TEST(PacketFlightRecorder, FreezeTriggerIsOffByDefaultAndClearedOnEnable) {
    PacketFlightRecorder recorder(8);
    recorder.setEnabled(true);
    recorder.record(Event::QuantumRefused, 1, 1, 1, 1, 1);
    EXPECT_FALSE(recorder.frozen()) << "no trigger was armed";

    recorder.setFreezeTrigger(Event::QuantumRefused);
    recorder.record(Event::QuantumRefused, 2, 2, 2, 2, 2);
    ASSERT_TRUE(recorder.frozen());

    // Re-enabling starts a fresh session, so a frozen buffer must thaw.
    recorder.setEnabled(true);
    EXPECT_FALSE(recorder.frozen());
    recorder.record(Event::PlaybackComplete, 3, 3, 3, 3, 3);
    EXPECT_EQ(recorder.recorded(), 1u);
}

// A 240 second run offers 186675 events into a 4096 slot buffer, nearly all of
// them routine completions, so the handful of anomalies a listener actually
// heard cannot all be present at once. Filtering keeps a whole run of them.
TEST(PacketFlightRecorder, EventMaskKeepsOnlyTheSelectedTypes) {
    PacketFlightRecorder recorder(64);
    recorder.setEventMask(PacketFlightRecorder::anomalyMask());
    recorder.setEnabled(true);

    for (uint32_t i = 0; i < 1000; ++i) {
        recorder.record(Event::PlaybackComplete, i, i, 0, 50, 0);
        recorder.record(Event::CaptureComplete, i, i, 0, 50, 0);
        recorder.record(Event::QuantumOffered, i, i, 64, 50, 0);
    }
    EXPECT_EQ(recorder.recorded(), 0u) << "routine traffic must not claim slots";

    recorder.record(Event::TransferDeferred, 1, 2, 0, 60, 96);
    recorder.record(Event::PlaybackUnderrun, 3, 4, 64, 0, 0);
    recorder.record(Event::QuantumRefused, 5, 6, 64, 180, 96);
    EXPECT_EQ(recorder.recorded(), 3u);

    PacketFlightRecorder::Record out[8]{};
    ASSERT_EQ(recorder.snapshot(out, 8), 3u);
    EXPECT_EQ(out[0].event, Event::TransferDeferred);
    EXPECT_EQ(out[1].event, Event::PlaybackUnderrun);
    EXPECT_EQ(out[2].event, Event::QuantumRefused);
}

TEST(PacketFlightRecorder, ZeroMaskRecordsEveryEventType) {
    PacketFlightRecorder recorder(64);
    recorder.setEventMask(0);
    recorder.setEnabled(true);
    recorder.record(Event::PlaybackComplete, 1, 1, 0, 0, 0);
    recorder.record(Event::QuantumRefused, 2, 2, 0, 0, 0);
    EXPECT_EQ(recorder.recorded(), 2u);
}

// The mask is configuration, so a new session must not silently start
// recording everything again.
TEST(PacketFlightRecorder, EventMaskSurvivesReEnabling) {
    PacketFlightRecorder recorder(64);
    recorder.setEventMask(PacketFlightRecorder::maskOf(Event::QuantumRefused));
    recorder.setEnabled(true);
    recorder.record(Event::PlaybackComplete, 1, 1, 0, 0, 0);
    ASSERT_EQ(recorder.recorded(), 0u);

    recorder.setEnabled(false);
    recorder.setEnabled(true);
    recorder.record(Event::PlaybackComplete, 2, 2, 0, 0, 0);
    EXPECT_EQ(recorder.recorded(), 0u) << "the mask was lost on re-enable";
    recorder.record(Event::QuantumRefused, 3, 3, 0, 0, 0);
    EXPECT_EQ(recorder.recorded(), 1u);
}

} // namespace
