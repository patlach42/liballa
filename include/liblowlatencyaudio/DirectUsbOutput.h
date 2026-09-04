/*
 * Copyright (C) 2026 patlach42
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef NNAGA_DIRECT_USB_OUTPUT_H
#define NNAGA_DIRECT_USB_OUTPUT_H

#include "liblowlatencyaudio/libusb_uac_driver.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <string>
#include <chrono>
#include <cmath>
#include <vector>

namespace guitarrackcraft {

// Full-duplex custom USB UAC bridge. The render thread uses bounded,
// lock-free playback and capture rings; lifecycle stays on control threads.
class DirectUsbOutput {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBitsPerSample = 32;
    static constexpr int kChannels = 2;
    static constexpr int kMaxDeviceChannels = monotrypt::usb::kMaxTransportChannels;
    static constexpr int kMaxSubslotBytes = monotrypt::usb::kMaxSubslotBytes;
    // Keep engine blocks within the driver's bounded playback watermark.
    static constexpr int kMaxGraphQuantum = monotrypt::usb::kMaxGraphQuantum;
    static constexpr int kMaxFramesPerWrite = kMaxGraphQuantum;

    DirectUsbOutput() = default;
    ~DirectUsbOutput() { stop(); close(); }

    bool open(int fd, int driverCode = 0) {
        if (fd < 0) return false;
        stop();
        close();
        if (!driver_.ensureContext() || !driver_.open(fd, driverCode)) return false;
        accepting_.store(false, std::memory_order_release);
        return true;
    }

    void close() {
        stop();
        driver_.close();
    }
    bool configureUserspaceBuffers(
            const monotrypt::usb::UserspaceBufferConfig& config) {
        return driver_.configureUserspaceBuffers(config);
    }

    bool start(int sampleRate, int bitsPerSample, int bytesPerSample, int channels,
               int outputPair) {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            startErrorDetail_.clear();
        }
        if (!driver_.isOpen() || sampleRate <= 0 ||
            (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) ||
            bytesPerSample < (bitsPerSample + 7) / 8 ||
            bytesPerSample > kMaxSubslotBytes ||
            channels < kChannels || channels > kMaxDeviceChannels ||
            outputPair < 0 || outputPair * 2 + 1 >= channels) return false;
        stop();
        if (!driver_.startDuplex(sampleRate, bitsPerSample, channels, bytesPerSample)) {
            std::lock_guard<std::mutex> lock(errorMutex_);
            startErrorDetail_ = driver_.lastErrorDetail();
            return false;
        }
        formatBits_ = bitsPerSample;
        formatBytes_ = bytesPerSample;
        deviceChannels_ = driver_.currentFormat().channels;
        const auto& capture = driver_.currentCaptureFormat();
        if (deviceChannels_ < kChannels || deviceChannels_ > kMaxDeviceChannels ||
            capture.channels <= 0 || capture.channels > kMaxDeviceChannels ||
            (capture.bitsPerSample != 16 && capture.bitsPerSample != 24 &&
             capture.bitsPerSample != 32) ||
            capture.bytesPerSample < (capture.bitsPerSample + 7) / 8 ||
            capture.bytesPerSample > kMaxSubslotBytes ||
            outputPair * 2 + 1 >= deviceChannels_) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                startErrorDetail_ =
                    "negotiated USB format is incompatible with the selected channels";
            }
            driver_.stop();
            return false;
        }
        outputPair_ = outputPair;
        // The render thread must fill the ring before startPlayback() arms OUT.
        accepting_.store(true, std::memory_order_release);
        playbackQuantumDrops_.store(0, std::memory_order_relaxed);
        streaming_.store(true, std::memory_order_release);
        return true;
    }

    bool startPlayback() noexcept {
        if (driver_.startPlayback()) return true;
        std::lock_guard<std::mutex> lock(errorMutex_);
        startErrorDetail_ = driver_.lastErrorDetail();
        return false;
    }

    int lastErrorCode() const noexcept {
        return static_cast<int>(driver_.lastError());
    }
    std::string lastErrorDetail() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return startErrorDetail_.empty() ? driver_.lastErrorDetail() : startErrorDetail_;
    }

    int startupPrimeFrames() const noexcept {
        return driver_.startupPrimeFrames();
    }
    uint64_t queuedOutFrames() const noexcept {
        return driver_.queuedOutFrames();
    }
    int captureTransferFrames() const noexcept {
        return driver_.captureTransferFrames();
    }
    int playbackTargetFrames() const noexcept {
        return driver_.playbackTargetFrames();
    }
    int captureTargetFrames() const noexcept {
        return driver_.captureTargetFrames();
    }
    int captureHeadroomFrames() const noexcept {
        return driver_.captureHeadroomFrames();
    }
    int captureDeadlineSlackFrames() const noexcept {
        return driver_.captureDeadlineSlackFrames();
    }

    void requestStop() noexcept {
        accepting_.store(false, std::memory_order_release);
        streaming_.store(false, std::memory_order_release);
        driver_.requestStop();
    }

    int captureAvailableFrames() const noexcept {
        return driver_.captureAvailableFrames();
    }
    int captureCapacityFrames() const noexcept {
        return driver_.captureCapacityFrames();
    }
    int captureChannelCount() const noexcept {
        return driver_.captureChannelCount();
    }

    std::vector<monotrypt::usb::UsbFormatCandidate> enumerateFormats() {
        return driver_.enumerateFormats();
    }

    void stop() {
        accepting_.store(false, std::memory_order_release);
        while (activeWriters_.load(std::memory_order_acquire) != 0) {
            // Control thread only: the render thread never enters this path.
            std::this_thread::yield();
        }
        streaming_.store(false, std::memory_order_release);
        driver_.stop();
    }

    bool isStreaming() const {
        return streaming_.load(std::memory_order_acquire) && driver_.isStreaming();
    }
    bool adapterStreaming() const noexcept {
        return streaming_.load(std::memory_order_acquire);
    }
    void resetRealtimeCounters() noexcept {
        driver_.resetRealtimeCounters();
    }
    bool driverStreaming() const noexcept {
        return driver_.isStreaming();
    }

    // Called only from the dedicated render thread. Admits one complete
    // quantum without waiting. A full or partially writable ring drops the
    // newest quantum; no partial commit is ever published.
    bool submitWholeQuantum(const float* left, const float* right,
                            int frames) noexcept {
        if (!left || !right || frames <= 0 ||
            frames > kMaxFramesPerWrite ||
            !accepting_.load(std::memory_order_acquire)) {
            return false;
        }
        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        bool submitted = false;
        const int writable = driver_.writableFrames();
        if (accepting_.load(std::memory_order_acquire) && writable >= frames) {
            const auto region = driver_.preparePlaybackWrite(frames);
            if (region.frames == frames &&
                packPlaybackRegionForFormat(region, left, right)) {
                driver_.commitPlaybackWrite(region);
                submitted = true;
            }
        }
        if (submitted) inspectContinuity(left, right, frames);
        activeWriters_.fetch_sub(1, std::memory_order_release);
        if (!submitted) playbackQuantumDrops_.fetch_add(1, std::memory_order_relaxed);
        // Both outcomes are recorded: a refusal alone says only that admission
        // failed, while the surrounding accepted blocks give the occupancy
        // sawtooth the refusal sits on top of. That relationship is what the
        // headroom policy has to be derived from.
        auto& recorder = driver_.flightRecorder();
        recorder.record(
            submitted
                ? monotrypt::usb::PacketFlightRecorder::Event::QuantumOffered
                : monotrypt::usb::PacketFlightRecorder::Event::QuantumRefused,
            monotrypt::usb::monotonicNowNs(),
            static_cast<uint32_t>(writable < 0 ? 0 : writable),
            static_cast<uint32_t>(frames),
            static_cast<uint32_t>(std::max(0, driver_.bufferedFrames())),
            static_cast<uint32_t>(driver_.queuedOutFrames()));
        return submitted;
    }

    uint64_t playbackQuantumDrops() const noexcept {
        return playbackQuantumDrops_.load(std::memory_order_relaxed);
    }

    // Reads all negotiated capture channels as normalized channel-major planes.
    int readInputChannels(float* const* destinations, int destinationChannels,
                          int frames) noexcept {
        if (!destinations || destinationChannels <= 0 || frames <= 0) return 0;
        frames = std::min(frames, kMaxFramesPerWrite);
        for (int channel = 0; channel < destinationChannels; ++channel) {
            if (!destinations[channel]) return 0;
            std::memset(destinations[channel], 0,
                        static_cast<size_t>(frames) * sizeof(float));
        }
        const auto region = driver_.prepareCaptureRead(frames);
        const auto& format = driver_.currentCaptureFormat();
        const int available = std::max(0, format.channels);
        const int decodedFrames = std::max(0, std::min(region.frames, frames));
        const int decodeChannels = std::min(destinationChannels, available);
        for (int channel = 0; channel < decodeChannels; ++channel) {
            const int sampleOffset = channel * format.bytesPerSample +
                (format.bytesPerSample - (format.bitsPerSample + 7) / 8);
            switch (format.bitsPerSample) {
                case 16: unpackCaptureChannel<16>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                case 24: unpackCaptureChannel<24>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                case 32: unpackCaptureChannel<32>(region, sampleOffset,
                    destinations[channel], decodedFrames); break;
                default: break;
            }
        }
        driver_.commitCaptureRead(region);
        // The loopback need not arrive on the first channel: an interface with
        // an internal loop returns it on the pair that the playback pair feeds.
        // If the requested channel is not among the decoded ones, skip and
        // count it rather than inspecting a neighbour: output attributed to a
        // channel nobody selected is worse than no output at all.
        const int inspect = captureInspectChannel_.load(std::memory_order_relaxed);
        if (inspect < decodeChannels) {
            inspectCapture(destinations[inspect], decodedFrames);
            inspectCaptureLevel(destinations[inspect], decodedFrames);
        } else if (decodeChannels > 0) {
            captureInspectSkips_.fetch_add(1, std::memory_order_relaxed);
        }
        return decodedFrames;
    }

    bool waitForCaptureFrames(int frames, int timeoutMs) const noexcept {
        return driver_.waitForCaptureFrames(frames, timeoutMs);
    }
    bool waitForCaptureUntil(
            int frames, std::chrono::steady_clock::time_point deadline) const noexcept {
        return driver_.waitForCaptureFramesUntil(frames, deadline);
    }
    bool waitForWritableFrames(int frames, int timeoutMs) const noexcept {
        return driver_.waitForWritableFrames(frames, timeoutMs);
    }
    int discardCaptureFrames(int frames) noexcept {
        return driver_.discardCaptureFrames(frames);
    }

    uint64_t xrunCount() const noexcept {
        return driver_.playbackXRunCount();
    }
    // Deferral causes and the smallest runway seen, so a run is attributable
    // from telemetry alone rather than from a flight recorder dump.
    uint64_t deferredNoMetadataCount() const noexcept {
        return driver_.deferredNoMetadataCount();
    }
    uint64_t deferredNoPcmCount() const noexcept {
        return driver_.deferredNoPcmCount();
    }
    uint64_t queuedOutLowWaterFrames() const noexcept {
        return driver_.queuedOutLowWaterFrames();
    }
    uint64_t playbackShortPacketCount() const noexcept {
        return driver_.playbackShortPacketCount();
    }
    uint64_t playbackShortFrameCount() const noexcept {
        return driver_.playbackShortFrameCount();
    }
    uint64_t playbackBackpressureCount() const noexcept {
        return driver_.playbackBackpressureCount();
    }
    void setUserspaceBufferConfig(
            int frames,
            const monotrypt::usb::UserspaceBufferConfig& config,
            int periodMultiplier = monotrypt::usb::kDefaultPeriodMultiplier) noexcept {
        driver_.setUserspaceBufferConfig(frames, config, periodMultiplier);
    }
    void setGraphQuantum(
            int frames,
            int periodMultiplier = monotrypt::usb::kDefaultPeriodMultiplier,
            int watermarkFrames = 0) noexcept {
        driver_.setGraphQuantum(frames, periodMultiplier, watermarkFrames);
    }
    int bufferedFrames() const noexcept { return driver_.bufferedFrames(); }
    int writableFrames() const noexcept { return driver_.writableFrames(); }
    uint64_t captureXRunCount() const noexcept {
        const auto stats = driver_.captureStats();
        return stats.overruns + stats.underruns;
    }
    uint64_t capturePacketDropCount() const noexcept {
        return driver_.capturePacketDropCount();
    }
    monotrypt::usb::CaptureStats captureStats() const noexcept {
        return driver_.captureStats();
    }
    monotrypt::usb::ImplicitFeedbackStats transportStats() const noexcept {
        return driver_.implicitFeedbackStats();
    }
    long writtenFrames() const noexcept { return driver_.writtenFrames(); }
    long playedFrames() const noexcept { return driver_.playedFrames(); }
    int32_t eventThreadTid() const noexcept {
        return driver_.eventThreadTid();
    }

    // Diagnostics: step 1 of the ladder in docs/measurement.md. Enable before
    // start; snapshot from a control thread once production has stopped.
    void setFlightRecorderEnabled(bool enabled) noexcept {
        driver_.flightRecorder().setEnabled(enabled);
    }
    // Flag a step between consecutive output samples larger than this fraction
    // of full scale. Zero disables the check.
    //
    // A click is a discontinuity in the signal, and the signal passes through
    // here, so it can be detected without a listener sitting through a four
    // minute tone. A 440 Hz tone at 48 kHz steps by at most 0.058 between
    // samples; anything far above that is a break.
    void setDiscontinuityThreshold(float threshold) noexcept {
        discontinuityThreshold_.store(
            threshold > 0.0f ? threshold : 0.0f, std::memory_order_release);
    }

    // Flag the captured level wandering from its own running average by more
    // than this fraction. Zero disables the check.
    //
    // A steady tone must come back steady. Measured as RMS over a window long
    // enough to be phase-independent: a 64 frame block covers only 59% of a
    // 440 Hz period, so its peak swings with phase alone and would be useless
    // as an envelope.
    void setCaptureModulationThreshold(float threshold) noexcept {
        captureModulationThreshold_.store(
            threshold > 0.0f ? threshold : 0.0f, std::memory_order_release);
    }

    // Which decoded capture channel the loopback detectors watch. Zero based;
    // a channel the format does not decode is not inspected at all, and the
    // skipped blocks are counted rather than redirected to a neighbour.
    void setCaptureInspectChannel(int channel) noexcept {
        captureInspectChannel_.store(channel < 0 ? 0 : channel,
                                     std::memory_order_relaxed);
        // The detectors carry state between blocks. Kept across a switch, the
        // first block of the new channel is compared against the last sample
        // and the running level of the old one, which reports a break and a
        // level swing that never happened.
        capturePrevious_ = 0.0f;
        capturePeak_ = 0.0f;
        captureSeeded_ = false;
        levelSum_ = 0.0;
        levelCount_ = 0;
        levelWindows_ = 0;
        levelReference_ = 0.0f;
    }
    int captureInspectChannel() const noexcept {
        return captureInspectChannel_.load(std::memory_order_relaxed);
    }
    uint64_t captureInspectSkips() const noexcept {
        return captureInspectSkips_.load(std::memory_order_relaxed);
    }

    // The same check on captured input, as a fraction of the signal's own
    // decaying peak so it is independent of input gain. With a loopback from
    // output one to input one this covers the DAC, the cable and the ADC.
    void setCaptureDiscontinuityThreshold(float threshold) noexcept {
        captureDiscontinuityThreshold_.store(
            threshold > 0.0f ? threshold : 0.0f, std::memory_order_release);
    }

    // The same check on the packed PCM leaving the ring, which together with
    // the one above brackets the packing and the two-span ring copy.
    void setTransferDiscontinuityThreshold(float threshold) noexcept {
        driver_.setTransferDiscontinuityThreshold(threshold);
    }

    // Record only the selected event types; zero records everything.
    void setFlightRecorderEventMask(uint32_t mask) noexcept {
        driver_.flightRecorder().setEventMask(mask);
    }
    // Freeze the recorder when this event first occurs, preserving its run-up.
    void setFlightRecorderFreezeTrigger(
            monotrypt::usb::PacketFlightRecorder::Event trigger) noexcept {
        driver_.flightRecorder().setFreezeTrigger(trigger);
    }
    bool flightRecorderFrozen() const noexcept {
        return driver_.flightRecorder().frozen();
    }
    bool flightRecorderEnabled() const noexcept {
        return driver_.flightRecorder().enabled();
    }
    uint64_t flightRecorderRecorded() const noexcept {
        return driver_.flightRecorder().recorded();
    }
    uint64_t flightRecorderDropped() const noexcept {
        return driver_.flightRecorder().dropped();
    }
    size_t flightRecorderSnapshot(
            monotrypt::usb::PacketFlightRecorder::Record* out,
            size_t capacity) const noexcept {
        return driver_.flightRecorder().snapshot(out, capacity);
    }

private:
    // Envelope of the captured signal, as RMS over a window spanning many
    // periods so it does not follow the waveform's own phase. A steady tone
    // returning at a wandering level means the output is modulated - which is
    // what summing with a delayed copy at a drifting delay produces, and what
    // a listener described as the wave overlapping itself.
    void inspectCaptureLevel(const float* samples, int frames) noexcept {
        const float threshold =
            captureModulationThreshold_.load(std::memory_order_relaxed);
        if (threshold <= 0.0f || !samples || frames <= 0 ||
            !driver_.flightRecorder().enabled()) {
            return;
        }
        for (int frame = 0; frame < frames; ++frame) {
            const double sample = samples[frame];
            levelSum_ += sample * sample;
            if (++levelCount_ < kLevelWindowFrames) continue;

            const float level = static_cast<float>(
                std::sqrt(levelSum_ / static_cast<double>(levelCount_)));
            levelSum_ = 0.0;
            levelCount_ = 0;
            // Ignore silence: a relative deviation means nothing without one.
            if (level < 0.01f) {
                levelReference_ = level;
                levelWindows_ = 0;
                continue;
            }
            if (levelReference_ <= 0.0f) { levelReference_ = level; continue; }
            // Let the reference converge before judging anything against it.
            // While the signal ramps up, every window differs from a reference
            // that is still chasing it, which produced 52 spurious events in
            // the first seconds of a run.
            if (++levelWindows_ <= kLevelWarmupWindows) {
                levelReference_ += (level - levelReference_) * 0.5f;
                continue;
            }

            const float deviation =
                std::fabs(level - levelReference_) / levelReference_;
            if (deviation > threshold) {
                driver_.flightRecorder().record(
                    monotrypt::usb::PacketFlightRecorder::Event::
                        CaptureModulation,
                    monotrypt::usb::monotonicNowNs(),
                    static_cast<uint32_t>(deviation * 10000.0f),
                    static_cast<uint32_t>(level * 10000.0f),
                    static_cast<uint32_t>(levelReference_ * 10000.0f),
                    static_cast<uint32_t>(driver_.writtenFrames()));
            }
            // Slow reference so it tracks a genuine level change over seconds
            // without following the modulation being measured.
            levelReference_ += (level - levelReference_) * 0.05f;
        }
    }

    // Relative to a decaying peak rather than to full scale: the loopback level
    // depends on the device's output and input gain, which the driver does not
    // know. A 440 Hz tone steps by 5.8% of its own peak between samples, so a
    // threshold several times that is unambiguous at any gain.
    void inspectCapture(const float* samples, int frames) noexcept {
        const float threshold =
            captureDiscontinuityThreshold_.load(std::memory_order_relaxed);
        if (threshold <= 0.0f || !samples || frames <= 0 ||
            !driver_.flightRecorder().enabled()) {
            return;
        }
        for (int frame = 0; frame < frames; ++frame) {
            const float value = samples[frame];
            const float magnitude = value < 0.0f ? -value : value;
            if (magnitude > capturePeak_) capturePeak_ = magnitude;
            // About a second of decay at 48 kHz, so the reference tracks a
            // level change without following a single break.
            capturePeak_ *= 0.99998f;
            const float step = value > capturePrevious_
                ? value - capturePrevious_ : capturePrevious_ - value;
            // Ignore anything below a usable level. A relative step is
            // meaningless while the envelope is still climbing: the first run
            // reported three breaks during the quarter second of ramp-up, at
            // peaks of 0.002 to 0.015, and none of them were real.
            if (captureSeeded_ && capturePeak_ > 0.02f &&
                step > threshold * capturePeak_) {
                driver_.flightRecorder().record(
                    monotrypt::usb::PacketFlightRecorder::Event::
                        CaptureDiscontinuity,
                    monotrypt::usb::monotonicNowNs(),
                    static_cast<uint32_t>(step / capturePeak_ * 10000.0f),
                    static_cast<uint32_t>(frame),
                    static_cast<uint32_t>(capturePeak_ * 10000.0f),
                    static_cast<uint32_t>(driver_.writtenFrames()));
            }
            capturePrevious_ = value;
            captureSeeded_ = true;
        }
    }

    // Walks the block once comparing each sample with its predecessor, the
    // previous block's last sample included, so a break at a block boundary is
    // caught too. One subtract and compare per sample, and only while the
    // recorder is armed.
    void inspectContinuity(const float* left, const float* right,
                           int frames) noexcept {
        const float threshold =
            discontinuityThreshold_.load(std::memory_order_relaxed);
        if (threshold <= 0.0f || !driver_.flightRecorder().enabled()) return;
        const float* const channels[2] = {left, right};
        for (int channel = 0; channel < 2; ++channel) {
            float previous = lastSample_[channel];
            for (int frame = 0; frame < frames; ++frame) {
                const float value = channels[channel][frame];
                const float step = value > previous ? value - previous
                                                    : previous - value;
                if (step > threshold && continuitySeeded_) {
                    driver_.flightRecorder().record(
                        monotrypt::usb::PacketFlightRecorder::Event::
                            SignalDiscontinuity,
                        monotrypt::usb::monotonicNowNs(),
                        static_cast<uint32_t>(step * 10000.0f),
                        static_cast<uint32_t>(frame),
                        static_cast<uint32_t>(channel),
                        // Frames written since the session started. Its
                        // remainder modulo the loop length says whether the
                        // breaks land on clip wraps or fall anywhere.
                        static_cast<uint32_t>(
                            static_cast<uint64_t>(driver_.writtenFrames()) +
                            static_cast<uint64_t>(frame)));
                }
                previous = value;
            }
            lastSample_[channel] = previous;
        }
        continuitySeeded_ = true;
    }

    template <int Bits, int Bytes>
    static void packPcm(float value, uint8_t* out) noexcept {
        int32_t sample;
        if constexpr (Bits == 16) {
            sample = value >= 1.0f ? 32767 : value <= -1.0f ? -32768
                : static_cast<int32_t>(value * 32767.0f);
        } else if constexpr (Bits == 24) {
            sample = value >= 1.0f ? 0x7FFFFF : value <= -1.0f ? -0x800000
                : static_cast<int32_t>(value * 8388607.0f);
        } else {
            sample = value >= 1.0f ? std::numeric_limits<int32_t>::max()
                : value <= -1.0f ? std::numeric_limits<int32_t>::min()
                : static_cast<int32_t>(value * 2147483647.0f);
        }
        constexpr int validBytes = (Bits + 7) / 8;
        static_assert(Bytes >= validBytes && Bytes <= kMaxSubslotBytes,
                      "subslot must hold every valid sample byte");
        constexpr int shift = 8 * (Bytes - validBytes);
        const uint32_t subslot = static_cast<uint32_t>(sample) << shift;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        // Identical bytes to the loop below on a little-endian target, but as
        // one store instead of four. USB subslots are little-endian by spec.
        if constexpr (Bytes == 4) {
            std::memcpy(out, &subslot, sizeof(subslot));
            return;
        }
#endif
        for (int byte = 0; byte < Bytes; ++byte) {
            out[byte] = static_cast<uint8_t>(subslot >> (8 * byte));
        }
    }

    template <int Bits, int Bytes>
    void packStereoRun(
            uint8_t* destination, int frames,
            const float* left, const float* right) const noexcept {
        if (frames <= 0) return;
        const int frameStride = deviceChannels_ * Bytes;
        if (deviceChannels_ != kChannels) {
            std::memset(
                destination, 0,
                static_cast<size_t>(frames) * frameStride);
        }
        const size_t leftOffset =
            static_cast<size_t>(outputPair_ * 2) * Bytes;
        const size_t rightOffset = leftOffset + Bytes;
        for (int frame = 0; frame < frames; ++frame) {
            uint8_t* output =
                destination + static_cast<size_t>(frame) * frameStride;
            packPcm<Bits, Bytes>(left[frame], output + leftOffset);
            packPcm<Bits, Bytes>(right[frame], output + rightOffset);
        }
    }

    // Resolve the session-fixed subslot width once per quantum so the packed
    // store width is a compile-time constant instead of a per-sample loop bound.
    bool packPlaybackRegionForFormat(
            const monotrypt::usb::LibusbUacDriver::PlaybackWriteRegion& region,
            const float* left, const float* right) const noexcept {
        switch (formatBits_) {
            case 16: return packPlaybackRegionForBits<16>(region, left, right);
            case 24: return packPlaybackRegionForBits<24>(region, left, right);
            case 32: return packPlaybackRegionForBits<32>(region, left, right);
            default: return false;
        }
    }

    template <int Bits>
    bool packPlaybackRegionForBits(
            const monotrypt::usb::LibusbUacDriver::PlaybackWriteRegion& region,
            const float* left, const float* right) const noexcept {
        constexpr int validBytes = (Bits + 7) / 8;
        switch (formatBytes_) {
            case 2:
                if constexpr (validBytes <= 2) {
                    packPlaybackRegion<Bits, 2>(region, left, right);
                    return true;
                } else {
                    return false;
                }
            case 3:
                if constexpr (validBytes <= 3) {
                    packPlaybackRegion<Bits, 3>(region, left, right);
                    return true;
                } else {
                    return false;
                }
            case 4:
                packPlaybackRegion<Bits, 4>(region, left, right);
                return true;
            default:
                return false;
        }
    }

    template <int Bits, int Bytes>
    void packPlaybackRegion(
            const monotrypt::usb::LibusbUacDriver::PlaybackWriteRegion& region,
            const float* left, const float* right) const noexcept {
        const size_t stride = static_cast<size_t>(region.frameStride);
        const int firstFrames =
            static_cast<int>(region.firstBytes / stride);
        packStereoRun<Bits, Bytes>(region.first, firstFrames, left, right);

        int sourceFrame = firstFrames;
        size_t secondOffset = 0;
        const size_t splitBytes =
            region.firstBytes - static_cast<size_t>(firstFrames) * stride;
        if (splitBytes > 0) {
            uint8_t splitFrame[kMaxDeviceChannels * kMaxSubslotBytes]{};
            packStereoRun<Bits, Bytes>(
                splitFrame, 1, left + sourceFrame, right + sourceFrame);
            std::memcpy(
                region.first + static_cast<size_t>(firstFrames) * stride,
                splitFrame, splitBytes);
            std::memcpy(
                region.second, splitFrame + splitBytes, stride - splitBytes);
            ++sourceFrame;
            secondOffset = stride - splitBytes;
        }
        const int remaining = region.frames - sourceFrame;
        if (remaining > 0) {
            packStereoRun<Bits, Bytes>(
                region.second + secondOffset, remaining,
                left + sourceFrame, right + sourceFrame);
        }
    }


    template <int Bits>
    static float unpackPcm(
            const uint8_t* input, float scale) noexcept {
        if constexpr (Bits == 16) {
            const uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8);
            const uint32_t extended =
                (bits & 0x8000u) ? bits | 0xffff0000u : bits;
            return static_cast<float>(
                static_cast<int32_t>(extended)) / scale;
        } else if constexpr (Bits == 24) {
            uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8) |
                (static_cast<uint32_t>(input[2]) << 16);
            if (bits & 0x00800000u) bits |= 0xff000000u;
            return static_cast<float>(static_cast<int32_t>(bits)) / scale;
        } else {
            const uint32_t bits =
                static_cast<uint32_t>(input[0]) |
                (static_cast<uint32_t>(input[1]) << 8) |
                (static_cast<uint32_t>(input[2]) << 16) |
                (static_cast<uint32_t>(input[3]) << 24);
            return static_cast<float>(static_cast<int32_t>(bits)) / scale;
        }
    }

    template <int Bits>
    void unpackCaptureRun(
            const uint8_t* source, int frames, int frameStride,
            int sampleOffset, float* destination) const noexcept {
        constexpr float scale = Bits == 16 ? 32768.0f :
                                Bits == 24 ? 8388608.0f : 2147483648.0f;
        for (int frame = 0; frame < frames; ++frame) {
            destination[frame] = unpackPcm<Bits>(
                source + static_cast<size_t>(frame) * frameStride +
                    sampleOffset,
                scale);
        }
    }

    template <int Bits>
    void unpackCaptureChannel(
            const monotrypt::usb::LibusbUacDriver::CaptureReadRegion& region,
            int sampleOffset, float* destination, int frames) const noexcept {
        if (frames <= 0 || region.frameStride <= 0) return;
        const size_t stride = static_cast<size_t>(region.frameStride);
        const int firstFrames = std::min(frames,
            static_cast<int>(region.firstBytes / stride));
        unpackCaptureRun<Bits>(region.first, firstFrames, region.frameStride,
                               sampleOffset, destination);
        int out = firstFrames;
        const size_t splitBytes = region.firstBytes -
            static_cast<size_t>(firstFrames) * stride;
        size_t secondOffset = 0;
        if (splitBytes > 0 && out < frames) {
            uint8_t splitFrame[kMaxDeviceChannels * kMaxSubslotBytes]{};
            std::memcpy(splitFrame, region.first +
                static_cast<size_t>(firstFrames) * stride, splitBytes);
            std::memcpy(splitFrame + splitBytes, region.second,
                        stride - splitBytes);
            unpackCaptureRun<Bits>(splitFrame, 1, region.frameStride,
                                   sampleOffset, destination + out);
            ++out;
            secondOffset = stride - splitBytes;
        }
        if (out < frames) unpackCaptureRun<Bits>(region.second + secondOffset,
            frames - out, region.frameStride, sampleOffset, destination + out);
    }

    int deviceChannels_ = kChannels;
    int outputPair_ = 0;
    mutable std::mutex errorMutex_;
    std::string startErrorDetail_;
    monotrypt::usb::LibusbUacDriver driver_;
    int formatBits_ = kBitsPerSample;
    int formatBytes_ = 4;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> activeWriters_{0};
    std::atomic<uint64_t> playbackQuantumDrops_{0};
    std::atomic<float> discontinuityThreshold_{0.0f};
    std::atomic<int> captureInspectChannel_{0};
    // Blocks left uninspected because the requested channel was not decoded.
    std::atomic<uint64_t> captureInspectSkips_{0};
    std::atomic<float> captureDiscontinuityThreshold_{0.0f};
    std::atomic<float> captureModulationThreshold_{0.0f};
    // 4096 frames is about 37 periods of a 440 Hz tone at 48 kHz, enough for
    // the RMS to be independent of where the window falls in the waveform.
    static constexpr int kLevelWindowFrames = 4096;
    // About 1.7 s at 48 kHz, comfortably past any start-up ramp.
    static constexpr int kLevelWarmupWindows = 20;
    double levelSum_ = 0.0;
    int levelCount_ = 0;
    int levelWindows_ = 0;
    float levelReference_ = 0.0f;
    float capturePrevious_ = 0.0f;
    float capturePeak_ = 0.0f;
    bool captureSeeded_ = false;
    float lastSample_[2]{0.0f, 0.0f};
    bool continuitySeeded_ = false;
};

} // namespace guitarrackcraft

#endif // NNAGA_DIRECT_USB_OUTPUT_H
