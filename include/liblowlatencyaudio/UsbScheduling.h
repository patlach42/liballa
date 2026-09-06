#pragma once
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>

namespace monotrypt::usb {

constexpr int kMinGraphQuantum = 4;
constexpr int kMaxGraphQuantum = 1024;
constexpr std::size_t kPlaybackRingBytes = 1u << 16;
constexpr int kMaxTransportChannels = 8;
constexpr int kMaxSubslotBytes = 4;
constexpr int kWorstTransportFrameBytes =
    kMaxTransportChannels * kMaxSubslotBytes;


struct PlaybackWatermarkConfig {
    int graphQuantum;
    int targetFrames;
    int frameLimit;
};

// Every userspace latency term is either explicit or uses the documented
// automatic policy when its value is zero. Transfer geometry must be selected
// before the ISO pumps are allocated; the other fields are applied after
// stream negotiation.
// Requests exactly none of a term whose zero already means "derive one".
inline constexpr int kExplicitZeroFrames = -1;

// Resolves one such term: the sentinel means none, zero means the automatic
// value the caller computed, anything else is the exact request.
constexpr int resolveOptionalFrames(int requested, int automatic) noexcept {
    if (requested == kExplicitZeroFrames) return 0;
    return requested == 0 ? automatic : requested;
}

struct UserspaceBufferConfig {
    // Zero selects the documented automatic policy. Positive values are exact
    // requests; unsupported values fail startup instead of being raised.
    int playbackTargetFrames = 0;
    int startupPrimeFrames = 0;
    int writeHeadroomFrames = 0;
    int captureLimitFrames = 0;
    // Capture target is the post-read cushion. Zero selects generic automatic
    // resolution; positive values are retained exactly when they fit.
    //
    // kExplicitZeroFrames asks for none of it. Zero cannot say that, because
    // zero already means "derive one", and a sweep that cannot reach a real
    // zero cannot find where the reserve stops paying for itself: its bottom
    // arm silently repeats the automatic value.
    int captureTargetFrames = 0;
    int captureHeadroomFrames = 0;
    int captureDeadlineSlackFrames = 0;
    int transferCount = 0;
    int packetsPerTransfer = 0;
    size_t ringCapacityBytes = 0;
};
// Checked frame-budget arithmetic used when resolving a policy against a ring.
constexpr bool checkedFrameBudgetFits(
        int first, int second, int third, int capacity) noexcept {
    if (first < 0 || second < 0 || third < 0 || capacity < 0) return false;
    const int64_t total = static_cast<int64_t>(first) + second + third;
    return total <= capacity;
}

constexpr int checkedFrameSum(int first, int second) noexcept {
    if (first < 0 || second < 0 ||
        static_cast<int64_t>(first) + second >
            std::numeric_limits<int>::max()) {
        return 0;
    }
    return first + second;
}

// A render quantum is admitted only when every capture frame is present.
// Processing a partial quantum would expose readInputChannels' zero-filled
// tail as an audible discontinuity.
constexpr bool isCompleteCaptureQuantum(
        int availableFrames, int requiredFrames) noexcept {
    return requiredFrames > 0 && availableFrames >= requiredFrames;
}


constexpr int kDefaultPeriodMultiplier = 3;
constexpr int kMinPeriodMultiplier = 1;
constexpr int kMaxPeriodMultiplier = 8;
// Milliseconds to poll for until a deadline, rounded up. A deadline shorter
// than a millisecond used to round to zero and skip the wait entirely: the
// admission deadline is one quantum period, which at 32 frames is 0.667 ms, so
// below a 64 frame quantum the target gate never waited and every block went
// straight to the ceiling test - which is why the ring's operating point did
// not respond to the target, the headroom or the credit reserve. The poll
// takes milliseconds, so overshoot is bounded by one of them.
inline int pollMillisUntil(std::chrono::steady_clock::time_point deadline,
                           std::chrono::steady_clock::time_point now) noexcept {
    const auto remainingNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
    if (remainingNs <= 0) return 0;
    const long long ms = (remainingNs + 999999) / 1000000;
    return static_cast<int>(ms > 0 ? ms : 1);
}

inline int pollMillisUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    return pollMillisUntil(deadline, std::chrono::steady_clock::now());
}

constexpr int kMinPacketsPerTransfer = 1;
constexpr int kMaxPacketsPerTransfer = 8;

constexpr int packetsPerSecondForInterval(bool highSpeed,
                                          int bInterval) noexcept {
    const int hostPeriods = highSpeed ? 8000 : 1000;
    const int interval = highSpeed
        ? (1 << std::clamp(bInterval - 1, 0, 15))
        : std::max(1, bInterval);
    return std::max(1, hostPeriods / interval);
}

constexpr int packetsPerTransferForRate(int packetsPerSecond) noexcept {
    if (packetsPerSecond >= 8000) return 8;
    if (packetsPerSecond >= 4000) return 4;
    if (packetsPerSecond >= 2000) return 2;
    return 1;
}

constexpr int nominalTransferFrames(int sampleRate, int packetsPerTransfer,
                                    int packetsPerSecond) noexcept {
    if (sampleRate <= 0 || packetsPerTransfer <= 0 || packetsPerSecond <= 0) {
        return 0;
    }
    const int64_t numerator =
        static_cast<int64_t>(sampleRate) * packetsPerTransfer;
    return static_cast<int>(
        std::min<int64_t>(std::numeric_limits<int>::max(),
                          (numerator + packetsPerSecond - 1) /
                              packetsPerSecond));
}

inline int clampPeriodMultiplier(int multiplier) noexcept {
    return std::clamp(multiplier, kMinPeriodMultiplier, kMaxPeriodMultiplier);
}

constexpr int playbackWatermarkTransferCount(
        int inflightTransfers, int reserveTransfers,
        bool exactInFlightAccounting) noexcept {
    const int inflight = std::max(0, inflightTransfers);
    const int reserve = std::max(0, reserveTransfers);
    if (exactInFlightAccounting)
        return reserve;
    return inflight > std::numeric_limits<int>::max() - reserve
        ? std::numeric_limits<int>::max()
        : inflight + reserve;
}

// Automatic write headroom, derived from how coarsely the ring is drained.
//
// Admission requires `writable >= quantum`, that is `occupancy <= target +
// headroom - quantum`. Between two admissions the consumer normally removes
// `quantum` frames in `quantum / drainChunk` completions, but the completions
// are quantised: when only one lands, occupancy climbs by
// `quantum - drainChunk` in a single step.
//
// Measured on an Audient iD4 at 48 kHz with a 64-frame quantum: with four
// packets per transfer the drain chunk is 24 frames, occupancy before
// admission ran 81-128 against a threshold of 128, and a cycle that skipped a
// drain was refused ten frames short. With eight packets the chunk is 48, the
// same threshold left 91 frames of margin, and no admission was refused.
// Equating the headroom to the graph quantum ignored the drain granularity,
// which is what left the narrow geometry with fourteen frames of margin where
// it needed forty.
//
// So the ring must hold the block being admitted plus that worst-case step.
// A geometry that drains at least a whole quantum per completion keeps the
// previous behaviour.
constexpr int automaticWriteHeadroomFrames(int graphQuantum,
                                           int drainChunkFrames) noexcept {
    const int quantum = std::max(0, graphQuantum);
    if (drainChunkFrames <= 0) return quantum;
    const int step = quantum > drainChunkFrames ? quantum - drainChunkFrames : 0;
    return quantum > std::numeric_limits<int>::max() - step
        ? std::numeric_limits<int>::max()
        : quantum + step;
}

// Keep the requested number of graph quanta queued before admitting one more.
inline PlaybackWatermarkConfig playbackWatermarkConfig(
        int requestedFrames, int periodMultiplier = kDefaultPeriodMultiplier) {
    const int quantum = std::clamp(requestedFrames,
                                   kMinGraphQuantum,
                                   kMaxGraphQuantum);
    const int multiplier = clampPeriodMultiplier(periodMultiplier);
    const int target = std::min(kMaxGraphQuantum, quantum * multiplier);
    return {quantum, target, quantum + target};
}
inline int effectivePlaybackTargetFrames(int configured,
                                         int queuedTransferFrames) noexcept {
    return std::max(0, std::max(configured, queuedTransferFrames));
}

constexpr int resolvedPlaybackTargetFrames(
        int automaticTargetFrames, int manualTargetFrames,
        int graphQuantum, int maxTargetFrames) noexcept {
    (void)graphQuantum;
    const int maximum = std::max(0, maxTargetFrames);
    if (maximum == 0)
        return 0;
    const int automatic =
        std::min(maximum, std::max(0, automaticTargetFrames));
    if (manualTargetFrames <= 0)
        return automatic;
    // Explicit calibration/expert targets are exact when bounded by capacity.
    return std::min(maximum, manualTargetFrames);
}
constexpr uint64_t playbackRunwayNanoseconds(
        uint64_t queuedFrames, uint32_t sampleRate) noexcept {
    if (queuedFrames == 0 || sampleRate == 0) return 0;
    constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    const uint64_t wholeSeconds = queuedFrames / sampleRate;
    const uint64_t remainderFrames = queuedFrames % sampleRate;
    if (wholeSeconds > std::numeric_limits<uint64_t>::max() /
                           kNanosecondsPerSecond) {
        return std::numeric_limits<uint64_t>::max();
    }
    uint64_t runway = wholeSeconds * kNanosecondsPerSecond;
    const uint64_t remainderNs =
        (remainderFrames * kNanosecondsPerSecond) / sampleRate;
    if (runway > std::numeric_limits<uint64_t>::max() - remainderNs) {
        return std::numeric_limits<uint64_t>::max();
    }
    return runway + remainderNs;
}
constexpr int startupPlaybackPrimeFrames(
        int maxTarget, int exactInitialPacketFrames,
        int playbackTargetFrames) noexcept {
    if (maxTarget <= 0) return 0;
    return std::min(
        maxTarget,
        std::max(0, std::max(
            exactInitialPacketFrames,
            playbackTargetFrames)));
}

// Exact rational packet scheduler. Each next() returns floor((rate + remainder)/period)
// while retaining the remainder, so the long-run sum is exactly rate frames.
class RationalPacketScheduler {
public:
    void reset(uint32_t rate, uint32_t packetsPerSecond) {
        period_ = packetsPerSecond ? packetsPerSecond : 1;
        whole_ = rate / period_;
        fraction_ = rate % period_;
        remainder_ = 0;
    }
    uint32_t next() noexcept {
        uint32_t frames = whole_;
        if (remainder_ >= period_ - fraction_) {
            remainder_ = remainder_ - (period_ - fraction_);
            ++frames;
        } else {
            remainder_ += fraction_;
        }
        return frames;
    }
private:
    uint32_t whole_ = 0;
    uint32_t fraction_ = 0;
    uint32_t period_ = 1;
    uint32_t remainder_ = 0;
};

} // namespace monotrypt::usb
