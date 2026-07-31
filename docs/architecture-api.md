# Architecture and API

## Components

- **LibusbUacDriver** owns descriptor discovery, interface/alternate-setting selection, transfer pool, event thread, lifecycle, and bounded capture/playback transport.
- **UsbScheduling** computes packet sizes for non-implicit endpoints with an exact rational scheduler. It avoids floating-point drift.
- **DirectUsbOutput** adapts float graph blocks to PCM rings and back. Conversion writes/reads up to two ring spans directly, preserving complete-frame publication and zero-fill semantics for unavailable capture frames.

The public CMake alias is `lowlatencyaudio::lowlatencyaudio`. Existing namespaces and C++ API remain the compatibility surface.

## Data flow

```text
USB capture -> preallocated ISO completion -> capture SPSC ring -> DirectUsbOutput float
float graph -> DirectUsbOutput -> playback SPSC ring -> whole-transfer admission -> USB playback
```

The transfer pool is preallocated. Eventfd wakeups bridge state transitions to the event thread. Ring ownership is single-producer/single-consumer; atomics publish cursors with release/acquire ordering.

## Implicit feedback

The Audient topology has asynchronous OUT and asynchronous implicit-feedback IN with a shared clock source and no explicit feedback endpoint. Capture packet `actual_length` is converted to frames and stored FIFO. Each OUT transfer consumes the corresponding complete layout. Missing metadata or PCM defers the whole transfer; order is retained. Variable packet payload offsets use `libusb_get_iso_packet_buffer()` (cumulative descriptor lengths), not `_simple`.

## Lifecycle API expectations

Start and stop are explicit and idempotence is handled by the owner. Stop must halt production, cease submissions, reap transfers, wake and join threads, release interfaces, and close native state before the Java FD is closed. Detach and permission failures are terminal for that FD. Reconnect requires a fresh probe and fresh transfer pool.
