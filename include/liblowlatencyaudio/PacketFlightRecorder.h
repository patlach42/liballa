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
#include <cstddef>
#include <cstdint>

namespace monotrypt::usb {

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
    void setEnabled(bool enabled) noexcept {
        if (enabled) reset();
        enabled_.store(enabled, std::memory_order_release);
    }

    void reset() noexcept {
        for (size_t i = 0; i < capacity_; ++i) {
            records_[i].sequence = 0;
            records_[i].event = Event::Unknown;
        }
        nextSequence_.store(0, std::memory_order_release);
    }

    // Realtime path. Bounded, allocation-free, lock-free.
    void record(Event event, uint64_t timestampNs, uint32_t a, uint32_t b,
                uint32_t ringFrames, uint32_t queuedFrames) noexcept {
        if (!enabled_.load(std::memory_order_relaxed)) return;
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
};

} // namespace monotrypt::usb
