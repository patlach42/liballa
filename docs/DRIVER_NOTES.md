# Direct USB production and investigation notes

This package-scoped adaptation preserves the direct USB evidence from the consumer notes. RackGraph, plugin, JNI, Kotlin/UI, and AudioEngine ownership details are intentionally omitted.


## Current Audient iD4 MKII profile

- Device: Audient iD4 MKII (`VID 0x2708`, `PID 0x0009`), UAC2, `48 kHz`,
  24-bit PCM in 32-bit subslots, four capture and four playback channels.
- Graph quantum: `16` frames. The current Auto policy with period multiplier
  `3` resolves to a `408`-frame userspace target and a `424`-frame startup
  prime.
- USB pump: eight transfers of four high-speed packets. This keeps 32 packets,
  normally 192 frames or 4 ms, submitted to the kernel while retaining 0.5 ms
  completion granularity.
- The latest eight-cycle device run before the packet-layout fix measured
  9.02–10.21 ms of complete host queue and reported zero host xruns, silence
  padding, transfer errors, lifecycle failures, and deadline misses. The user
  still heard periodic silence clicks. This proves that the stress counters
  were not sufficient to establish end-to-end continuity.
- Host queue measurements cover capture → graph → playback queueing. They do
  not include ADC/DAC conversion, analog loopback, or the device's internal
  FIFO.

## Hardware clock topology

The live Android USB descriptor inventory reported:

```text
Playback: interface 1 alt 1, endpoint 0x01 OUT
          bmAttributes 0x05: asynchronous isochronous data
Capture:  interface 2 alt 1, endpoint 0x81 IN
          bmAttributes 0x25: asynchronous implicit-feedback data
Both:     wMaxPacketSize 208, bInterval 1

All terminals → Clock Selector 40 → Clock Source 41
```

There is no explicit feedback endpoint. Capture endpoint `0x81` is the implicit
feedback source for playback. The card is the clock master; software must pace
OUT packets from capture packet sizes rather than force a nominal 48,000-frame
host clock.

The driver follows this model:

1. Select capture and playback endpoints that resolve to the same clock source.
2. Start capture before playback and prime implicit metadata.
3. Convert every successful capture packet's `actual_length` to audio frames.
4. Preserve those per-packet frame counts in FIFO order.
5. Apply each capture-derived layout to the corresponding OUT transfer.
6. Defer an OUT transfer when a complete packet layout or complete PCM payload
   is unavailable; never substitute a nominal layout in implicit mode.

This matches the Linux `snd-usb-audio` model:

- [generic UAC2 implicit-feedback association](https://github.com/torvalds/linux/blob/master/sound/usb/implicit.c#L70-L120)
- [capture packet layout to playback packet layout](https://github.com/torvalds/linux/blob/master/sound/usb/endpoint.c#L1760-L1830)
- [pending OUT URBs wait for a capture-derived layout](https://github.com/torvalds/linux/blob/master/sound/usb/endpoint.c#L465-L500)

Microsoft's generic UAC2 driver does not support implicit feedback and is not a
reference for this device path:
[USB Audio 2.0 drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers).

## Confirmed periodic-click root cause

The implicit path correctly produced variable packet lengths such as 6/6/7/6
frames, but addressed packet payloads with
`libusb_get_iso_packet_buffer_simple()`.

Libusb explicitly restricts that helper to transfers where every packet has the
same length. It computes:

```text
packet address = transfer buffer + first packet length × packet index
```

After any 5- or 7-frame feedback packet, later PCM was written at the wrong
offset. Payload ranges overlapped and the transfer could send stale or
zero-initialized bytes while all USB descriptors, lengths, statuses, ring
counters, and xrun counters remained valid. The clock correction cadence made
the corruption sound periodic.

Both variable-length playback writers now use
`libusb_get_iso_packet_buffer()`, which sums every preceding packet descriptor
length. Do not restore the `_simple` helper unless all descriptor lengths are
provably identical for the complete transfer.

Regression coverage:

```text
UsbDriverImplicit.VariablePacketLengthsPreserveSubmittedPcmAcrossBoundaries
```

The test uses one three-packet transfer with frame counts `3/2/2`, descriptor
lengths `12/8/8`, and unique PCM bytes. It checks the complete submitted payload
byte-for-byte. The old helper places packet 3 at byte 24 instead of cumulative
byte 20 and fails this contract.

## Why there is no filler-packet setting

Do not expose a user setting for arbitrary empty or zero-filled packets:

- zero-length packets break the card-derived service cadence;
- zero-filled PCM produces the same signal-to-silence discontinuity heard as a
  click;
- extra packets change device FIFO occupancy without changing clock ownership;
- a successful transfer containing silence remains invisible to ordinary USB
  status/error counters.

The valid packet size must remain capture-derived. Latency and reliability
knobs may control userspace reserve or transfer runway, but must not fabricate
implicit-feedback layouts.

## Production cleanup after the root-cause fix

Keep:

- Audient-scoped eight-transfer/four-packet profile: 4 ms kernel runway with
  0.5 ms completion granularity.
- Capture-before-OUT metadata priming.
- Exact rational packet scheduler for non-implicit paths.
- Per-packet capture-derived layouts for implicit feedback.
- Whole-transfer PCM admission, deferred-transfer FIFO ordering, bounded ring
  writes, eventfd wakeups, urgent event thread, and explicit backpressure
  semantics.
- Separate counters for producer backpressure, capture errors, submitted
  silence, transfer errors, lifecycle failure, and implicit runway health.

Hardware verification on the Audient iD4 MKII used eight 30-second
start/run/stop cycles at 48 kHz, 24-bit PCM in 32-bit subslots, four channels,
buffer 16, and multiplier 3:

- 8/8 cycles passed; buffer 16 remained the minimum tested stable buffer;
- capture, playback, aggregate, and zero-runway xruns stayed at zero;
- transfer errors, metadata FIFO overruns, lifecycle failures, and deadline
  misses stayed at zero;
- deferred transfers were recovered in FIFO order; observed pending high-water
  was at most 5 of 8 transfers and maximum pending age was 2.40 ms;
- observed host queue estimate was 313-346 frames, or 6.52-7.21 ms. This is
  host-side accounting, not analog end-to-end latency.

## Explicit userspace buffering controls

The previous userspace watermark implementation combined a user value with
hidden safety floors. A positive watermark was raised to the automatic target,
the graph quantum was added during write admission, the Audient profile added a
hidden nine-transfer reserve, and the thermal policy could temporarily add two
graph quanta. This made the visible setting an unreliable latency control.

The current contract exposes the latency terms separately:

- **Playback target**: exact steady-state userspace playback target. `0` uses
  the documented automatic policy; a positive value is not raised by a device
  profile or thermal policy.
- **Startup prime**: exact prefill before the first OUT submission. `0` uses
  the automatic prime. It must still fit the physical ring and contain the
  negotiated initial packet descriptors.
- **Write headroom**: explicit admission headroom independent of the graph
  quantum. `0` selects the documented automatic headroom.
- **Capture queue limit**: explicit capture-read limit. `0` selects the derived
  transfer/playback runway policy.
- **Transfer count** and **packets per transfer**: `0` selects endpoint/profile
  geometry; positive values are bounded to `1..8` and validated before pump
  allocation. These are transport tuning controls, not guaranteed latency
  improvements.
- **Ring capacity**: both SPSC rings use the selected power-of-two byte capacity
  (`4..1024 KiB`) and apply it only at the next engine start. Resizing requires
  stopping the USB event/pump threads before replacing callback-visible storage;
  it is a physical storage ceiling, not an automatic latency target.

Invalid combinations fail startup rather than being silently raised. In
particular, `playback target + write headroom` must fit the selected ring, and
startup prime must fit the ring and the exact initial USB packet runway.

The thermal safety policy was removed from the default path so explicit queue
settings remain stable during a session. If reintroduced, it must be an
explicit setting with visible state and telemetry showing when it raises the
target. Performance hints may continue without changing queue geometry.

Native contract tests cover exact target/headroom admission and invalid target
combinations. APK assembly and the standalone liblowlatencyaudio test suite
were run after this change; physical-device verification requires an attached
ADB target.

## Measurement contract

A passing host stress run establishes only the observed invariants:

- capture/playback rings did not report starvation or overflow;
- libusb transfer and packet statuses completed successfully;
- no PCM silence was inserted by the measured drain path;
- render deadlines and lifecycle transitions passed;
- capture, graph-write, and playback-drain frame accounting remained
  conserved.

It does not establish:

- on-time USB microframe service when a later URB still completes normally;
- correct device FIFO occupancy;
- PLL/CLOCK_VALID continuity;
- analog output continuity;
- payload correctness unless packet-boundary bytes are tested;
- absence of a short pending-transfer runway gap.

For any remaining click, prefer a fixed-size packet-event flight recorder,
matched main/test APK device stress, Linux A/B, usbmon or a hardware USB
analyzer, and analog loopback with a deterministic signal. Do not infer
end-to-end continuity from aggregate xrun counters alone.

## Optimization constraints

- Do not use global `-ffast-math`, `-Ofast`, `-mcpu=native`, fixed SVE, or
  unverified `restrict`/alignment assumptions.
- ThinLTO, PGO, NEON specialization, vectorization pragmas, and thermal-aware
  policy are benchmark candidates, not assumed wins. Validate numerical output
  and callback p50/p95/p99, deadline misses, and xruns on arm64 hardware.
- `io_uring` does not replace USBFS `SUBMITURB`/reap. Realtime scheduler, IRQ
  affinity, cpufreq, usbfs sysctls, and kernel patches require privileges a
  normal Android app does not have.
- CPU affinity and urgent scheduler requests are best-effort. Android/vendor
  policy may ignore or deny them; they are never part of the correctness
  contract.
- Unknown USB devices retain the conservative generic transfer and watermark
  policy. Do not apply the Audient profile without device-specific hardware
  evidence.