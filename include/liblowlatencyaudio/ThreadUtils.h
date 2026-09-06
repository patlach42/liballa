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
#include <ctime>
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/resource.h>
#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif
namespace guitarrackcraft {

inline long getTid() {
#if defined(__ANDROID__) && defined(__linux__)
    return static_cast<long>(syscall(SYS_gettid));
#else
    return static_cast<long>(pthread_self());
#endif
}

// Wall time minus this over the same span is the time the thread was runnable
// and not running. Two vDSO reads, so it can bracket work on an audio thread
// without being the disturbance it is there to find.
inline uint64_t threadCpuNanoseconds() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

#if defined(__linux__)
// Select the two highest-ranked allowed CPUs without allocating or consulting
// system state. Direct USB render and event threads need separate performance
// cores; leaving the fastest core to UI work caused live-session starvation.
inline cpu_set_t deriveAudioCpuMask(const cpu_set_t& allowed) noexcept {
    cpu_set_t audio;
    CPU_ZERO(&audio);

    int allowedCount = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        allowedCount += CPU_ISSET(cpu, &allowed) ? 1 : 0;
    if (allowedCount == 0)
        return audio;

    const int firstRank = std::max(0, allowedCount - 2);
    const int lastRank = allowedCount - 1;
    int rank = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        if (rank >= firstRank && rank <= lastRank)
            CPU_SET(cpu, &audio);
        ++rank;
    }
    return audio;
}

// The pool above is two cores for two threads, and handing both threads the
// same pair leaves the placement to the scheduler, which is free to stack them
// on one core and leave the other idle. Naming a core per role is what the
// comment on the pool always intended. The render thread takes the
// highest-ranked core; USB servicing takes the other, because a completion the
// device is waiting on cannot be made up later. When the pool has only one CPU
// to give, both roles get it and the split is a no-op.
enum class AudioCpuRole { Render, Service };

// Affinity is switchable at runtime, and it exists for one reason: the syscall
// fix that made audio affinity work also made the UI affinity call in the
// activity work, so every comparison between "pinned" and "unpinned" moved two
// things at once and could attribute the difference to neither. One binary
// with both switchable is what separates them.
//
// Defaults reproduce what the driver does when nobody sets anything.
// ExclusiveSplit also narrows the render thread to the rest of the pool, so
// the two never contend. Measured worth revisiting once the render thread's
// own tail came down.
enum class ServiceCpuPlacement { OneCoreOfPool, WholePool, ExclusiveSplit };

inline std::atomic<bool>& audioAffinityEnabledFlag() noexcept {
    static std::atomic<bool> enabled{true};
    return enabled;
}
inline std::atomic<bool>& uiAffinityEnabledFlag() noexcept {
    static std::atomic<bool> enabled{true};
    return enabled;
}
// The whole pool, never one core of it. This platform runs core control on the
// prime cluster: with min_cpus of one and a busy threshold of sixty percent,
// it parks the second prime core whenever the cluster is quiet - and audio
// keeps it at about seventeen percent, so one of the two is parked nearly
// always. A thread pinned to one of them is runnable with nowhere to run every
// time the parked one is the one it is pinned to, which is where the
// millisecond runqueue waits came from. Pinning to the cluster leaves it
// somewhere to go.
inline std::atomic<int>& serviceCpuPlacementFlag() noexcept {
    static std::atomic<int> placement{
        static_cast<int>(ServiceCpuPlacement::WholePool)};
    return placement;
}

inline void setAudioAffinityEnabled(bool enabled) noexcept {
    audioAffinityEnabledFlag().store(enabled, std::memory_order_relaxed);
}
inline void setUiAffinityEnabled(bool enabled) noexcept {
    uiAffinityEnabledFlag().store(enabled, std::memory_order_relaxed);
}
inline void setServiceCpuPlacement(ServiceCpuPlacement placement) noexcept {
    serviceCpuPlacementFlag().store(static_cast<int>(placement),
                                    std::memory_order_relaxed);
}

// Only USB servicing is pinned, and only onto one core of the pool. The render
// thread is left wherever the platform puts it.
//
// The asymmetry is what measurement kept pointing at, and it follows from what
// each thread can recover from. A completion the device is waiting on cannot
// be made up later, and servicing does almost no work, so it wants one fast
// core and no migrations. The graph has a deadline every quantum and a peak
// far above its average cost, so every restriction placed on it made things
// worse: held to one core of the pool it starved in two cycles out of five,
// and moved below the pool - six cores at 3.63 GHz against the pool's 4.61 -
// its peak cycle went from 120 microseconds to 3.6 milliseconds against a
// 1.33 ms budget and every cycle failed.
//
// Leaving it unpinned is also the conservative reading of the platform's own
// advice, which is that applications should not normally set CPU affinity at
// all because topology and thermal behaviour vary by device. Servicing is the
// one place the measurement earns the exception.
inline cpu_set_t deriveAudioRoleCpuMask(const cpu_set_t& allowed,
                                        AudioCpuRole role) noexcept {
    const cpu_set_t pool = deriveAudioCpuMask(allowed);
    const auto placement = static_cast<ServiceCpuPlacement>(
        serviceCpuPlacementFlag().load(std::memory_order_relaxed));
    if (placement == ServiceCpuPlacement::ExclusiveSplit) {
        int lowest = -1;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &pool)) { lowest = cpu; break; }
        }
        if (lowest < 0 || CPU_COUNT(&pool) < 2) return pool;
        cpu_set_t chosen;
        CPU_ZERO(&chosen);
        if (role == AudioCpuRole::Service) {
            CPU_SET(lowest, &chosen);
        } else {
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &pool) && cpu != lowest) CPU_SET(cpu, &chosen);
            }
        }
        return chosen;
    }
    // The graph gets the fast pair and may migrate inside it. Leaving it
    // unpinned was measured and is not neutral: the platform places it on the
    // slow cluster and the peak cycle goes from about 120 microseconds to
    // between one and three and a half milliseconds against a 1.33 ms budget.
    if (role == AudioCpuRole::Render)
        return pool;
    if (placement == ServiceCpuPlacement::WholePool) return pool;
    int lowestPool = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &pool)) { lowestPool = cpu; break; }
    }
    if (lowestPool < 0)
        return allowed;
    cpu_set_t chosen;
    CPU_ZERO(&chosen);
    CPU_SET(lowestPool, &chosen);
    return chosen;
}

// Keep UI descendants away from the CPUs reserved for Direct USB audio. On
// constrained one/two-CPU cpusets preserve at least one runnable UI CPU.
inline cpu_set_t deriveUiCpuMask(const cpu_set_t& allowed) noexcept {
    cpu_set_t ui = allowed;
    const int allowedCount = CPU_COUNT(&allowed);
    if (allowedCount <= 0)
        return ui;
    if (allowedCount <= 2) {
        CPU_ZERO(&ui);
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) {
                CPU_SET(cpu, &ui);
                break;
            }
        }
        return ui;
    }
    const cpu_set_t audio = deriveAudioCpuMask(allowed);
    CPU_XOR(&ui, &allowed, &audio);
    return ui;
}

inline void applyCurrentThreadUiAffinity() noexcept {
    if (!uiAffinityEnabledFlag().load(std::memory_order_relaxed))
        return;
    // Two things the raw system call does not do for you, and the libc wrapper
    // does. It reports success by returning the number of bytes it copied into
    // the mask rather than zero, so testing for zero failed on every
    // successful call. And it writes only those bytes: the rest of a 128 byte
    // cpu_set_t keeps whatever was on the stack, which read back as CPUs that
    // do not exist and made the derived mask invalid. The first fault hid the
    // second by returning before anything was set.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (syscall(SYS_sched_getaffinity, 0, sizeof(allowed), &allowed) < 0)
        return;
    const cpu_set_t ui = deriveUiCpuMask(allowed);
    if (CPU_COUNT(&ui) == 0)
        return;
    (void)syscall(SYS_sched_setaffinity, 0, sizeof(ui), &ui);
}

inline void applyCurrentThreadAudioAffinity(AudioCpuRole role) noexcept {
    if (!audioAffinityEnabledFlag().load(std::memory_order_relaxed))
        return;
    // Two things the raw system call does not do for you, and the libc wrapper
    // does. It reports success by returning the number of bytes it copied into
    // the mask rather than zero, so testing for zero failed on every
    // successful call. And it writes only those bytes: the rest of a 128 byte
    // cpu_set_t keeps whatever was on the stack, which read back as CPUs that
    // do not exist and made the derived mask invalid. The first fault hid the
    // second by returning before anything was set.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (syscall(SYS_sched_getaffinity, 0, sizeof(allowed), &allowed) < 0)
        return;
    const cpu_set_t audio = deriveAudioRoleCpuMask(allowed, role);
    if (CPU_COUNT(&audio) == 0)
        return;
    // A role that asks for everything it already has is asking not to be
    // pinned. Setting the mask anyway would be a no-op today and a way to
    // freeze a stale cpuset tomorrow, so it is not set.
    if (CPU_EQUAL(&audio, &allowed))
        return;
    (void)syscall(SYS_sched_setaffinity, 0, sizeof(audio), &audio);
}
#endif

// Android Dynamic Performance Framework session, resolved lazily so the
// min-SDK 26 binary remains loadable on pre-API-31 devices.
class PerformanceHintSession {
public:
    explicit PerformanceHintSession(
            int64_t targetDurationNs,
            const int32_t* threadIds = nullptr,
            size_t threadCount = 0) noexcept {
#if defined(__ANDROID__)
        if (targetDurationNs <= 0) return;
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        getManager_ = reinterpret_cast<GetManagerFn>(
            dlsym(library_, "APerformanceHint_getManager"));
        createSession_ = reinterpret_cast<CreateSessionFn>(
            dlsym(library_, "APerformanceHint_createSession"));
        reportActual_ = reinterpret_cast<ReportActualFn>(
            dlsym(library_, "APerformanceHint_reportActualWorkDuration"));
        closeSession_ = reinterpret_cast<CloseSessionFn>(
            dlsym(library_, "APerformanceHint_closeSession"));
        setThreads_ = reinterpret_cast<SetThreadsFn>(
            dlsym(library_, "APerformanceHint_setThreads"));
        // Newer platforms let a session say it would rather have performance
        // than efficiency. Resolved dynamically like the rest, so an older
        // device simply does not get it.
        setPreferPowerEfficiency_ =
            reinterpret_cast<SetPreferPowerEfficiencyFn>(
                dlsym(library_, "APerformanceHint_setPreferPowerEfficiency"));
        // API 35 lets a session report the wall time a work period took and the
        // CPU time inside it as separate numbers. That distinction is the whole
        // story for an audio callback: the old single-number call could only be
        // given the CPU cost, which for this graph is about ten microseconds
        // against a deadline of thirteen hundred - a workload the system is
        // entitled to place anywhere and clock down. Resolved dynamically like
        // the rest, so an older device simply keeps the old signal.
        workDurationCreate_ = reinterpret_cast<WorkDurationCreateFn>(
            dlsym(library_, "AWorkDuration_create"));
        workDurationRelease_ = reinterpret_cast<WorkDurationReleaseFn>(
            dlsym(library_, "AWorkDuration_release"));
        workDurationSetStart_ = reinterpret_cast<WorkDurationSetI64Fn>(
            dlsym(library_, "AWorkDuration_setWorkPeriodStartTimestampNanos"));
        workDurationSetTotal_ = reinterpret_cast<WorkDurationSetI64Fn>(
            dlsym(library_, "AWorkDuration_setActualTotalDurationNanos"));
        workDurationSetCpu_ = reinterpret_cast<WorkDurationSetI64Fn>(
            dlsym(library_, "AWorkDuration_setActualCpuDurationNanos"));
        reportActual2_ = reinterpret_cast<ReportActual2Fn>(
            dlsym(library_, "APerformanceHint_reportActualWorkDuration2"));
        if (!getManager_ || !createSession_ || !reportActual_ || !closeSession_) {
            dlclose(library_);
            library_ = nullptr;
            return;
        }
        void* manager = getManager_();
        const int32_t currentTid = static_cast<int32_t>(getTid());
        const int32_t* initialThreadIds =
            threadIds && threadCount > 0 ? threadIds : &currentTid;
        const size_t initialThreadCount =
            threadIds && threadCount > 0 ? threadCount : 1U;
        if (manager) {
            session_ = createSession_(
                manager, initialThreadIds, initialThreadCount,
                targetDurationNs);
        }
        // Audio has a fixed deadline every quantum, so a clock chosen for
        // battery life is the wrong trade here: missing the deadline costs a
        // dropout, and there is no way to make that up later.
        if (session_ && setPreferPowerEfficiency_) {
            (void)setPreferPowerEfficiency_(session_, false);
        }
        // One work-duration object for the life of the session: the render
        // thread fills it in and hands it over each period, so nothing is
        // allocated on the audio path.
        if (session_ && workDurationCreate_ && workDurationRelease_ &&
            workDurationSetStart_ && workDurationSetTotal_ &&
            workDurationSetCpu_ && reportActual2_) {
            workDuration_ = workDurationCreate_();
        }
#else
        (void)targetDurationNs;
        (void)threadIds;
        (void)threadCount;
#endif
    }

    ~PerformanceHintSession() {
#if defined(__ANDROID__)
        if (workDuration_ && workDurationRelease_) workDurationRelease_(workDuration_);
        if (session_ && closeSession_) closeSession_(session_);
        if (library_) dlclose(library_);
#endif
    }

    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    bool active() const noexcept {
#if defined(__ANDROID__)
        return session_ != nullptr;
#else
        return false;
#endif
    }

    // APerformanceHint_setThreads landed in API 34; older platforms resolve
    // the symbol to null and can never rebind, which is a different answer
    // from a rebind the platform actively refused.
    bool canSetThreads() const noexcept {
#if defined(__ANDROID__)
        return session_ != nullptr && setThreads_ != nullptr;
#else
        return false;
#endif
    }

    bool setThreads(const int32_t* tids, size_t count) noexcept {
#if defined(__ANDROID__)
        return session_ && setThreads_ && tids && count > 0 &&
               setThreads_(session_, tids, count) == 0;
#else
        (void)tids;
        (void)count;
        return false;
#endif
    }

    bool canReportWorkDuration() const noexcept {
#if defined(__ANDROID__)
        return workDuration_ != nullptr;
#else
        return false;
#endif
    }

    // Wall time the period took and the CPU time inside it. Telling the system
    // both is what lets it see a deadline the work is nowhere near consuming
    // but must not miss, which one number could never express.
    void reportWorkDuration(int64_t startTimestampNs, uint64_t totalNs,
                            uint64_t cpuNs) noexcept {
#if defined(__ANDROID__)
        if (!workDuration_ || totalNs == 0) return;
        workDurationSetStart_(workDuration_, startTimestampNs);
        workDurationSetTotal_(workDuration_, static_cast<int64_t>(totalNs));
        workDurationSetCpu_(workDuration_, static_cast<int64_t>(
            cpuNs > 0 ? cpuNs : 1));
        (void)reportActual2_(session_, workDuration_);
#else
        (void)startTimestampNs;
        (void)totalNs;
        (void)cpuNs;
#endif
    }

    void reportActualWorkDuration(uint64_t durationNs) noexcept {
#if defined(__ANDROID__)
        if (session_ && durationNs > 0) {
            (void)reportActual_(session_, static_cast<int64_t>(durationNs));
        }
#else
        (void)durationNs;
#endif
    }

private:
#if defined(__ANDROID__)
    using GetManagerFn = void* (*)();
    using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
    using ReportActualFn = int (*)(void*, int64_t);
    using SetThreadsFn = int (*)(void*, const int32_t*, size_t);
    using SetPreferPowerEfficiencyFn = int (*)(void*, bool);
    using CloseSessionFn = void (*)(void*);
    using WorkDurationCreateFn = void* (*)();
    using WorkDurationReleaseFn = void (*)(void*);
    using WorkDurationSetI64Fn = void (*)(void*, int64_t);
    using ReportActual2Fn = int (*)(void*, void*);

    void* library_ = nullptr;
    void* session_ = nullptr;
    GetManagerFn getManager_ = nullptr;
    CreateSessionFn createSession_ = nullptr;
    ReportActualFn reportActual_ = nullptr;
    SetThreadsFn setThreads_ = nullptr;
    SetPreferPowerEfficiencyFn setPreferPowerEfficiency_ = nullptr;
    CloseSessionFn closeSession_ = nullptr;
    void* workDuration_ = nullptr;
    WorkDurationCreateFn workDurationCreate_ = nullptr;
    WorkDurationReleaseFn workDurationRelease_ = nullptr;
    WorkDurationSetI64Fn workDurationSetStart_ = nullptr;
    WorkDurationSetI64Fn workDurationSetTotal_ = nullptr;
    WorkDurationSetI64Fn workDurationSetCpu_ = nullptr;
    ReportActual2Fn reportActual2_ = nullptr;
#endif
};

class ThermalHeadroomMonitor {
public:
    ThermalHeadroomMonitor() noexcept {
#if defined(__ANDROID__)
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return;
        acquire_ = reinterpret_cast<AcquireFn>(
            dlsym(library_, "AThermal_acquireManager"));
        release_ = reinterpret_cast<ReleaseFn>(
            dlsym(library_, "AThermal_releaseManager"));
        getHeadroom_ = reinterpret_cast<GetHeadroomFn>(
            dlsym(library_, "AThermal_getThermalHeadroom"));
        if (acquire_ && release_ && getHeadroom_)
            manager_ = acquire_();
        if (!manager_) {
            dlclose(library_);
            library_ = nullptr;
        }
#endif
    }

    ~ThermalHeadroomMonitor() {
#if defined(__ANDROID__)
        if (manager_ && release_) release_(manager_);
        if (library_) dlclose(library_);
#endif
    }

    ThermalHeadroomMonitor(const ThermalHeadroomMonitor&) = delete;
    ThermalHeadroomMonitor& operator=(const ThermalHeadroomMonitor&) = delete;

    float sample(int forecastSeconds) const noexcept {
#if defined(__ANDROID__)
        return manager_ && getHeadroom_
            ? getHeadroom_(manager_, forecastSeconds)
            : -1.0f;
#else
        (void)forecastSeconds;
        return -1.0f;
#endif
    }

private:
#if defined(__ANDROID__)
    using AcquireFn = void* (*)();
    using ReleaseFn = void (*)(void*);
    using GetHeadroomFn = float (*)(void*, int);
    void* library_ = nullptr;
    void* manager_ = nullptr;
    AcquireFn acquire_ = nullptr;
    ReleaseFn release_ = nullptr;
    GetHeadroomFn getHeadroom_ = nullptr;
#endif
};

// Best-effort Android/Linux realtime scheduling for app-owned audio threads.
// Prefer SCHED_FIFO when permitted, then Android's urgent-audio nice level.
// Call only once at thread startup; failure is exposed through diagnostics.
inline bool setCurrentThreadUrgentAudio(const char* name,
                                        AudioCpuRole role) noexcept {
    if (name != nullptr) {
        (void)pthread_setname_np(pthread_self(), name);
    }
#if defined(__linux__)
    applyCurrentThreadAudioAffinity(role);
#else
    (void)role;
#endif
#if defined(__ANDROID__) && defined(__linux__)
    sched_param realtime{};
    realtime.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &realtime) == 0)
        return true;
    constexpr int kUrgentAudioNice = -19;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(getTid()),
                       kUrgentAudioNice) == 0;
#else
    return false;
#endif
}

} // namespace guitarrackcraft
