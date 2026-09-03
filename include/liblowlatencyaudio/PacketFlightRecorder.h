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
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace monotrypt::usb {

// One clock for every recorder producer, so records from the USB completion
// path and from the render thread share a timeline.
inline uint64_t monotonicNowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}


// Fixed-size packet-event flight recorder, step 1 of the diagnostics ladder in
// docs/measurement.md.
//
// Why this exists. Aggregate counters cannot resolve the events we care about.
// The same configuration measured twelve producer quantum drops in one campaign
// and four in the next, with barely overlapping host-queue ranges, because an
// eight-by-thirty-second run does not resolve an event that occurs once in
// fifteen thousand quanta. Ranking configurations by such counters ranks noise.
// This records what actually happened at each event instead of how many times
// something happened.
//
// Realtime contract. Recording is a bounded store into preallocated storage:
// no allocation, no lock, no syscall, no logging, no unbounded loop. Storage is
// sized at construction and never grows. When the buffer wraps, the oldest
// records are overwritten and `dropped()` counts what was lost, so a full
// recorder degrades by losing history rather than by blocking a producer.
//
// Ownership. Multiple realtime producers (the USB completion path and the
// render thread) may record concurrently; the reader is a control thread that
// snapshots after the stream is stopped. Slot claiming is a relaxed atomic
// increment, and each record carries its own sequence so a reader can detect a
// slot that was still being written when the snapshot was taken.
class PacketFlightRecorder {
public:
    enum class Event : uint16_t {
        Unknown = 0,
        // A playback transfer completed and was refilled from the ring.
        PlaybackComplete = 1,
        // A render quantum was offered to the playback ring.
        QuantumOffered = 2,
        // A render quantum was refused because the ring was at its limit.
        // `a` carries the writable frames, `b` the requested frames.
        QuantumRefused = 3,
        // A capture transfer completed. `a` carries the frames admitted.
        CaptureComplete = 4,
        // drainRing could not satisfy a packet and padded with silence.
        // `a` carries the frames served, `b` the frames requested.
        PlaybackUnderrun = 5,
        // A playback transfer was deferred awaiting implicit metadata or PCM.
        TransferDeferred = 6,
    };

    struct Record {
        uint64_t sequence = 0;      // monotonic; 0 means never written
        uint64_t timestampNs = 0;
        uint32_t a = 0;             // event-specific, see Event
        uint32_t b = 0;
        uint32_t ringFrames = 0;    // playback ring occupancy at the event
        uint32_t queuedFrames = 0;  // frames already handed to USB
        Event event = Event::Unknown;
        uint16_t reserved = 0;
    };

    // Capacity is rounded up to a power of two so the slot index is a mask
    // rather than a division on the realtime path.
    explicit PacketFlightRecorder(size_t requestedCapacity = 4096) {
        size_t capacity = 1;
        while (capacity < requestedCapacity && capacity < (size_t{1} << 20))
            capacity <<= 1;
        capacity_ = capacity;
        mask_ = capacity - 1;
        records_ = new Record[capacity];
    }

    ~PacketFlightRecorder() { delete[] records_; }

    PacketFlightRecorder(const PacketFlightRecorder&) = delete;
    PacketFlightRecorder& operator=(const PacketFlightRecorder&) = delete;

    size_t capacity() const noexcept { return capacity_; }

    bool enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    // Setup-time only. Enabling clears the history so a session never inherits
    // records from the previous one.
    // The event mask and freeze trigger are configuration, not history, so
    // they survive this; only the records and the frozen state are cleared.
    void setEnabled(bool enabled) noexcept {
        if (enabled) reset();
        frozen_.store(false, std::memory_order_release);
        enabled_.store(enabled, std::memory_order_release);
    }

    // Record only these event types. The mask is a bitfield of
    // `1 << static_cast<uint16_t>(Event)`; zero records everything.
    //
    // Without a filter the anomalies drown. A 240 second run offers 186675
    // events into a 4096 slot buffer, almost all of them routine completions,
    // so the five deferred transfers and the one refusal that a listener
    // actually heard cannot all be present at once. Recording only the
    // anomalous types keeps a whole run's worth of them, with timestamps that
    // can be matched against when a click was heard.
    //
    // Setup-time only.
    void setEventMask(uint32_t mask) noexcept {
        eventMask_.store(mask, std::memory_order_release);
    }

    static constexpr uint32_t maskOf(Event event) noexcept {
        return uint32_t{1} << static_cast<uint16_t>(event);
    }

    // The events that indicate something went wrong, as opposed to the
    // steady-state traffic that surrounds them.
    static constexpr uint32_t anomalyMask() noexcept {
        return maskOf(Event::QuantumRefused) |
               maskOf(Event::PlaybackUnderrun) |
               maskOf(Event::TransferDeferred);
    }

    // Freeze the buffer the first time `trigger` is recorded, the way an
    // aircraft recorder preserves the run-up to an incident.
    //
    // Without this the instrument cannot see what it exists to see. Keeping the
    // newest records is the wrong policy for a rare event: a thirty second run
    // offers about 143000 events, the buffer holds 4096, and the one refusal
    // worth explaining is evicted by the steady-state traffic that follows it.
    // Freezing keeps the refusal as the newest record and every event leading
    // up to it as context.
    //
    // Event::Unknown disables the trigger. Setup-time only.
    void setFreezeTrigger(Event trigger) noexcept {
        freezeTrigger_.store(trigger, std::memory_order_release);
    }

    bool frozen() const noexcept {
        return frozen_.load(std::memory_order_acquire);
    }

    void reset() noexcept {
        for (size_t i = 0; i < capacity_; ++i) {
            records_[i].sequence = 0;
            records_[i].event = Event::Unknown;
        }
        nextSequence_.store(0, std::memory_order_release);
        frozen_.store(false, std::memory_order_release);
    }

    // Realtime path. Bounded, allocation-free, lock-free.
    void record(Event event, uint64_t timestampNs, uint32_t a, uint32_t b,
                uint32_t ringFrames, uint32_t queuedFrames) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
        if (frozen_.load(std::memory_order_relaxed)) return;
        const uint32_t mask = eventMask_.load(std::memory_order_relaxed);
        if (mask != 0 && (mask & maskOf(event)) == 0) return;
        const uint64_t sequence =
            nextSequence_.fetch_add(1, std::memory_order_relaxed);
        Record& slot = records_[sequence & mask_];
        // Publish the payload before the sequence: a reader that sees the new
        // sequence must not see a half-written record.
        slot.timestampNs = timestampNs;
        slot.a = a;
        slot.b = b;
        slot.ringFrames = ringFrames;
        slot.queuedFrames = queuedFrames;
        slot.event = event;
        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence = sequence + 1;
        // Freeze after publishing, so the trigger itself is the newest record.
        // A concurrent producer may still land one or two records behind it;
        // that is preferable to a lock on the realtime path.
        if (event == freezeTrigger_.load(std::memory_order_relaxed))
            frozen_.store(true, std::memory_order_release);
    }

    // Total events offered to the recorder, including those overwritten.
    uint64_t recorded() const noexcept {
        return nextSequence_.load(std::memory_order_acquire);
    }

    // Events lost to wrap-around.
    uint64_t dropped() const noexcept {
        const uint64_t total = recorded();
        return total > capacity_ ? total - capacity_ : 0;
    }

    // Control thread only, after production has stopped. Copies at most
    // `capacity` records in chronological order and returns how many were
    // written. Records whose sequence is zero were never populated.
    size_t snapshot(Record* out, size_t capacity) const noexcept {
        if (!out || capacity == 0) return 0;
        const uint64_t total = recorded();
        const uint64_t available = total < capacity_ ? total : capacity_;
        const uint64_t first = total - available;
        const uint64_t wanted = available < capacity ? available : capacity;
        // Take the newest `wanted` records so a small buffer keeps the tail.
        const uint64_t begin = first + (available - wanted);
        for (uint64_t i = 0; i < wanted; ++i) {
            std::atomic_thread_fence(std::memory_order_acquire);
            out[i] = records_[(begin + i) & mask_];
        }
        return static_cast<size_t>(wanted);
    }

private:
    Record* records_ = nullptr;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    std::atomic<uint64_t> nextSequence_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> frozen_{false};
    std::atomic<Event> freezeTrigger_{Event::Unknown};
    std::atomic<uint32_t> eventMask_{0};
};

} // namespace monotrypt::usb
