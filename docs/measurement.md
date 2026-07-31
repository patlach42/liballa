# Measurement, diagnostics, and calibration

## What telemetry proves

A passing stress run proves only the measured invariants: capture/playback rings did not report starvation or overflow; libusb transfer and packet statuses completed; no explicit silence was inserted; render deadlines and lifecycle transitions passed; frame accounting remained conserved. It does **not** prove USB microframe timing, device FIFO/PLL continuity, analog output continuity, packet payload correctness, or absence of a brief pending-transfer runway gap.

The host queue estimate is capture → graph → playback queueing. It excludes ADC/DAC conversion, device FIFO, analog loopback, and acoustic latency. Report it with frames, sample rate, and the explicit label “estimated host queue,” never as analog round-trip latency.

## Repeatable calibration

Record device identity, descriptors and clock topology, sample rate, valid bits/subslot bytes, channel count, block size, period multiplier, transfer runway, watermark, cycle count, and Android build/device. Run at least two 30-second cycles per candidate profile, including stop/restart. Require zero capture/playback/aggregate xruns, transfer errors, lifecycle failures, deadline misses, metadata FIFO overruns, and unexplained silence before lowering a watermark. Manual watermark cannot undercut the automatic safety floor.

For every accepted profile retain raw telemetry and summarize minimum stable buffer, host queue frames/ms, pending-transfer high-water/age, and failure counters. Unknown devices require their own evidence.

## Diagnostics ladder

For a remaining click or discontinuity:

1. Capture a fixed-size packet-event flight recorder with packet lengths, cumulative offsets, metadata FIFO depth, pending age, and wakeups.
2. Reproduce with the main/test APK and compare a Linux A/B run.
3. Inspect usbmon or a hardware USB analyzer for service cadence and payload boundaries.
4. Use a deterministic impulse and analog loopback to measure actual round-trip latency.

Never infer end-to-end continuity from aggregate xrun counters alone. Test variable packet boundaries byte-for-byte; `_simple` offset assumptions can corrupt payload while statuses remain successful.

## Analyzer

`tools/analyze_direct_usb_telemetry.py` parses `TELEMETRY` and `AUDIT_SUMMARY` records, validates lifecycle coverage and stable cycles, and reports estimated host queue latency. It is copied from the consumer unchanged; its latency field is intentionally not analog latency.
