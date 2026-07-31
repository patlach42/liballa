# Agent guide: liblowlatencyaudio

## Scope and ownership

This repository owns the autonomous C++17 direct-USB UAC driver pack: `LibusbUacDriver`, `UsbScheduling`, `DirectUsbOutput`, USB lifecycle, bounded rings, transfer scheduling, and diagnostics. It does not own AudioEngine, RackGraph, plugins, JNI, Kotlin/UI, or VST code. Keep the public target `lowlatencyaudio::lowlatencyaudio` and existing C++ namespaces/API stable.

The Android `UsbDeviceConnection` FD is borrowed. The owner must keep the Java connection alive through native stop, transfer reap, thread joins, interface release, and native close. Capture starts before playback for implicit feedback. Detach, permission revocation, and FD failure must drive an orderly stop/reap/join/close sequence; never close an FD while transfers or event threads can still use it.

## Realtime prohibitions

Audio callbacks and USB completion paths are hard realtime-adjacent code:

- no heap allocation/free, exceptions, blocking locks, filesystem/network I/O, logging, sleeps, JNI calls, or unbounded loops;
- no fabricated zero/silence packets to hide missing implicit metadata;
- publish only complete PCM frames and admit only complete transfers;
- use bounded lock-free SPSC rings, preallocated ISO storage, eventfd wakeups, and exact rational scheduling;
- variable-length ISO payloads require cumulative `libusb_get_iso_packet_buffer()` offsets; `_simple` is forbidden unless equal lengths are proven.

Urgent scheduler requests, affinity, ADPF, prefaulting, and optional bounded `mlock` are best effort or setup-time only. Never assume Android grants realtime privileges, IRQ affinity, cpufreq, usbfs sysctls, kernel patches, `mlockall`, or fixed SVE/native CPU tuning.

## Device evidence policy

The Audient iD4 profile is device-scoped and evidence-backed. Unknown devices keep conservative generic transfer and watermark policy. Do not generalize a profile, lower a watermark floor, or claim analog latency without packet-layout evidence and repeated physical-device cycles. Host queue estimates are not ADC/DAC, device FIFO, or analog round-trip measurements.

## Ownership and lifecycle

One component owns each ring cursor; release/acquire publication must preserve complete-frame visibility. Transfer pool slots, metadata FIFO, eventfd, and worker threads have explicit owners. Stop production first, stop/reap transfers, issue stop wakeup, join event/RT threads, release interfaces, then native close; Java closes `UsbDeviceConnection` last. Reconnect only after old state is quiescent.

## Build and test commands

From a host checkout, configure/build the library with CMake and system libusb; Android builds use the bundled pinned libusb submodule and USBFS backend. Typical commands (adjust generator/toolchain as needed):

```sh
cmake -S . -B build -DLOWLATENCYAUDIO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/analyze_direct_usb_telemetry.py --required-cycles 2 telemetry.log
```

Do not add new tests for documentation-only changes. Existing tests are the contract for ring wrap, PCM formats, scheduling, implicit feedback, lifecycle, and realtime invariants. Run focused tests when changing code; do not claim host tests establish analog continuity.

## Change and documentation obligations

Before changing an exported symbol, inspect all references and preserve compatibility. Update `docs/DRIVER_NOTES.md` when observed behavior, device evidence, telemetry schema, packet layout, lifecycle ordering, or calibration changes. Update architecture/Android/measurement docs when integration contracts change. Every performance claim must include hardware/profile, block/buffer, multiplier, sample rate, metric, and failure counts. Keep rejected approaches explicit: no global fast-math/Ofast/native/SVE, no io_uring substitution for USBFS, no unmeasured batching or wakeup coalescing, and no rack/plugin-specific material in this driver pack.

When proposing transfer batching or coalesced eventfd wakeups, first prove no lost wakeups, FIFO violations, disconnect races, or stop/restart leaks. ADPF must report CPU work rather than USB wait; PGO generation instrumentation never ships. Recalibrate the manual watermark only after a verified stable reduction and never below the automatic safety floor.

Do not commit, publish, or rewrite the bundled libusb history. Keep its pinned commit unchanged unless an explicit dependency update is requested.
