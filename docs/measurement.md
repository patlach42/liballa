# Measurement, diagnostics, and calibration

## What telemetry proves

A passing stress run proves only the measured invariants: capture/playback rings did not report starvation or overflow; libusb transfer and packet statuses completed; no explicit silence was inserted; render deadlines and lifecycle transitions passed; frame accounting remained conserved. It does **not** prove USB microframe timing, device FIFO/PLL continuity, analog output continuity, packet payload correctness, or absence of a brief pending-transfer runway gap.

The host queue estimate is capture → graph → playback queueing. It excludes ADC/DAC conversion, device FIFO, analog loopback, and acoustic latency. Report it with frames, sample rate, and the explicit label “estimated host queue,” never as analog round-trip latency.

## Repeatable calibration

Record device identity, descriptors and clock topology, sample rate, valid
bits/subslot bytes, channel count, graph quantum, period multiplier, transfer
runway, playback target, capture target/headroom/slack, and Android build/device.
Use a short ascending scan only to nominate a profile. Certify the selected
profile with one continuous five-minute run plus repeated stop/start cycles.
Require zero capture/playback/aggregate xruns, packet or quantum drops, transfer
errors, lifecycle failures, metadata FIFO overruns, and inserted silence.
Scheduler pressure is reported separately and must remain inside the measured
capture/playback runway; it is not itself an audible discontinuity.

Positive expert targets are exact. Lowering an automatic target therefore
requires profile-specific device evidence rather than a hidden driver floor.
Retain raw telemetry and summarize the minimum stable quantum, host queue
frames/ms, pending-transfer high-water/age, and failure counters.

## Deferred transfers are audible

A deferred OUT transfer is the correct response to missing capture metadata or
PCM - the driver must never fabricate an implicit layout - but the device still
loses that service slot, and a listener hears it.

Two four-minute runs of the same profile on an Audient iD4, with a listener
present: six deferrals plus one admission refusal against eight clicks reported,
and on the next run two anomalies against one click. Every other counter -
playback xruns, inserted silence, zero-runway events, capture drops, metadata
FIFO overruns - stayed at zero throughout, so the aggregate gate passed a
configuration that was plainly not clean.

Certification therefore requires zero deferral growth alongside the other
counters. A profile whose only fault is deferrals is not a passing profile.

## Diagnostics ladder

For a remaining click or discontinuity:

1. Capture a fixed-size packet-event flight recorder with packet lengths, cumulative offsets, metadata FIFO depth, pending age, and wakeups.
2. Reproduce with the main/test APK and compare a Linux A/B run.
3. Inspect usbmon or a hardware USB analyzer for service cadence and payload boundaries.
4. Use a deterministic impulse and analog loopback to measure actual round-trip latency.

The Direct USB settings **Measure round-trip** action performs that final step.
With output 1 physically connected to input 1, it temporarily mutes the rack,
emits a bounded deterministic probe on output 1, captures input 1, and reports
the normalized-correlation delay in frames and milliseconds. This result is
full DAC → cable → ADC analog round trip for the active format/profile. Keep it
separate from the estimated host queue. Correlation and input/output peaks must
be reported with the latency so a muted, clipped, unplugged, or weak loop cannot
produce a credible result.

Never infer end-to-end continuity from aggregate xrun counters alone. Test variable packet boundaries byte-for-byte; `_simple` offset assumptions can corrupt payload while statuses remain successful.

## Analyzer

`tools/analyze_direct_usb_telemetry.py` parses `TELEMETRY` and `AUDIT_SUMMARY` records, validates lifecycle coverage and stable cycles, and reports estimated host queue latency. It is copied from the consumer unchanged; its latency field is intentionally not analog latency.
