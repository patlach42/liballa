# liblowlatencyaudio
NNAGA (NNAGA Not Android Guitar App) owns and maintains this driver pack; project identity and ownership are attributed to patlach42.

Autonomous C++17 Android direct-USB UAC low-latency driver pack. The public CMake target is `lowlatencyaudio::lowlatencyaudio`; it contains `LibusbUacDriver`, `UsbScheduling`, and the float `DirectUsbOutput` adapter. AudioEngine, RackGraph, plugins, JNI, Kotlin/UI, and VST hosts are intentionally outside this repository.

## Clone and initialize

```bash
# The repository is currently local-only. After publishing its canonical remote:
git clone --recurse-submodules <liblowlatencyaudio-remote>
# Existing clone:
git submodule update --init --recursive
```

Android builds require the pinned `third_party/libusb` submodule. The CMake
configure step fails with the initialization command when it is absent.

## Build and link

```cmake
add_subdirectory(path/to/liblowlatencyaudio)
target_link_libraries(app PRIVATE lowlatencyaudio::lowlatencyaudio)
```

On Android the root project builds the pinned bundled libusb USBFS backend (submodule commit `578ab76b4c434f8b204137ab6d7310689c7a9704`). Host tests use system libusb. Do not add a second driver or libusb target in the consumer.

Standalone contract tests are opt-in:

```bash
cmake -S . -B build -DLOWLATENCYAUDIO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The test configure fetches immutable GoogleTest commit
`f8d7d77c06936315286eb55f8de22cd23c188571` and therefore needs network access
unless FetchContent is already populated. Host `usb_driver_tests` additionally
require `pkg-config` and `libusb-1.0`; without them the scheduling and Linux CPU
affinity contracts still build, while driver lifecycle tests are reported as
disabled.


The include tree preserves the existing C++ namespaces and API. Only ownership and include paths change when integrating as a subdirectory or local git submodule.

## Android integration contract

1. Enumerate with `UsbManager`, identify the selected `UsbDevice`, and request user permission. Do not open a device before the permission broadcast is granted.
2. Create `UsbDeviceConnection`, claim the required UAC interfaces/alternate settings, and pass the resulting file descriptor to the native driver while the Java connection remains strongly reachable. The descriptor is borrowed: closing or garbage-collecting the connection invalidates USBFS operations. Keep the connection alive until `stop()` and `close()` have completed.
3. Disable Android USB-audio routing for this path; the direct USB driver owns the interfaces and clocking. Do not run AudioTrack/AudioRecord on the same device.
4. Start capture first. Prime implicit-feedback packet metadata, then submit playback transfers. For stop/restart, stop graph production, stop/reap transfers, wake and join event/RT threads, release interfaces, then close the native driver and finally close `UsbDeviceConnection`.
5. Treat permission revocation, detach, FD errors, and claimed-interface failures as lifecycle failures. Reconnect only after all old transfers and threads are quiescent.

## Architecture and real-time invariants

- Bounded lock-free SPSC rings carry capture/playback PCM. Producers publish complete frames only; consumers never observe partial frames.
- USB transfers use a preallocated isochronous pool. No allocation, blocking mutex, unbounded loop, logging, or filesystem work is allowed on the realtime callback or USB completion path.
- `eventfd` wakeups connect bounded capture/playback state changes to waiting threads. Stop signals those waiters; libusb event-thread exit is bounded by cancellation callbacks and its event timeout.
- Playback admission is whole-transfer: defer an OUT transfer unless all PCM and (for implicit feedback) all packet metadata are available. Deferred transfers retain FIFO order and are never padded with fabricated layouts or silence.
- Implicit-feedback devices use capture-derived packet layout FIFO. Capture starts before playback; each successful capture packet's frame count is paired with the corresponding OUT packet.
- Variable packet lengths must use `libusb_get_iso_packet_buffer()`, never `_simple`, because `_simple` assumes equal descriptor lengths and corrupts payload offsets.
- Non-implicit paths use an exact rational packet scheduler; do not replace it with floating-point accumulation.
- Urgent scheduling, CPU affinity, and performance hints are best effort. Android/vendor policy may deny them; correctness never depends on them.

The DirectUsbOutput float adapter converts directly into up to two writable/readable ring spans (zero-copy two-span PCM). The old staging vectors are not part of the driver contract.

## Device policy and observed result

The Audient iD4 MKII profile is device-scoped (`VID 0x2708`, `PID 0x0009`, UAC2, 48 kHz, 24-bit in 32-bit subslots, four channels). Its eight-transfer/four-packet runway and safety watermark are evidence-backed, not universal defaults. Unknown devices retain conservative generic transfer and watermark policy; do not generalize the Audient profile without packet-layout, clock, and live-cycle evidence.

Under the documented Audient configuration (48 kHz, buffer 16, multiplier 3, eight 30-second cycles), observed host queue estimates were **313–346 frames (6.52–7.21 ms)** with zero measured xruns, transfer errors, lifecycle failures, and deadline misses. This is host queue accounting only: it excludes ADC/DAC conversion, device FIFO, analog loopback, and acoustic latency. A Xiaomi live profile also demonstrated a stable 48-frame/multiplier-5 run at an estimated 8.4375 ms host queue; that profile is not a universal promise.

## Performance decisions and rejected approaches

The accepted path uses bounded rings, preallocated ISO buffers, eventfd wakeups, whole-transfer admission, exact scheduling, and zero-copy two-span PCM. ARM64 isolated measurements showed playback conversion about 8% faster at 32 frames and 6–10% faster at 256 frames; capture was neutral at 32 and up to about 6% faster at 256. Live gains remain profile/device dependent and require fresh calibration.

Transfer batching (reserve/copy/publish once per transfer) and coalesced eventfd wakeups (signal transitions rather than every completion) are research candidates. They require lost-wakeup, disconnect, wrap, and contention tests before adoption.

ADPF reporting must describe actual CPU work, not USB wait/backpressure. Choose performance CPUs from observed capacity/frequency and retain permission fallbacks. Prefault bounded buffers off the RT thread; optional `mlock` is limited by `RLIMIT_MEMLOCK`; never use `mlockall`. PGO requires representative production profiles and instrumentation must not ship.

Rejected as generic solutions: global `-ffast-math`/`-Ofast`, `-mcpu=native`, fixed SVE or unverified NEON assumptions, broad allocator replacement, `io_uring` in place of USBFS `SUBMITURB`/reap, and privileged realtime/IRQ/cpufreq/usbfs-kernel tuning. Validate every optimization on physical ARM64 with callback p50/p95/p99, deadline misses, xruns, thermal and power data.

## Measurement and diagnostics

A passing stress run establishes only ring starvation/overflow counters, libusb statuses, explicit silence insertion, deadline/lifecycle transitions, and conserved frame accounting. It does not prove microframe timing, device FIFO/PLL continuity, analog output continuity, packet payload correctness, or absence of a short pending-transfer runway gap.

Use `tools/analyze_direct_usb_telemetry.py` for structured logcat records and `docs/measurement.md` for calibration, flight-recorder, usbmon/hardware-analyzer, deterministic impulse, and analog-loopback methodology. Always record format, buffer, multiplier, cycles, host queue label, and whether analog latency was measured. Recalibrate the minimum stable watermark after each verified change; only a lower stable queue with zero failures is a latency improvement.

## Licensing

The driver pack is GPL-3.0-or-later; see `LICENSE`. Bundled libusb remains
LGPL-2.1-or-later under its upstream notices and
`third_party/libusb/COPYING`. Preserve both license sets and comply with their
distribution requirements when shipping binaries.

## Source notes

`docs/DRIVER_NOTES.md` records the observed Audient topology, implicit-feedback fix, lifecycle and telemetry evidence, and optimization constraints. Claims in this README are intentionally limited to those notes and the cited measurement contract.
