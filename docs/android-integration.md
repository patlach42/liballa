# Android integration

This page is the integration checklist for an application embedding the driver as a CMake subdirectory. It intentionally describes the driver boundary only; application graph/UI/JNI policy remains in the consumer.

## Permission and descriptor setup

Use `UsbManager` to enumerate and request permission for the selected `UsbDevice`. Wait for the permission broadcast before opening it. Construct `UsbDeviceConnection`, claim the UAC interfaces and required alternate settings, then pass its file descriptor to native code. The FD is borrowed and valid only while the Java connection remains open and strongly reachable. A closed connection invalidates USBFS transfers even if the integer FD value remains unchanged.

Disable Android USB-audio routing while direct USB owns the interfaces. Do not concurrently open AudioTrack/AudioRecord for the same device.

## Start ordering

Probe descriptors and clock-source associations first. For implicit-feedback devices:

1. Start capture.
2. Receive and queue capture packet frame counts (the clock master's layout).
3. Prime enough metadata and PCM in bounded rings.
4. Submit playback transfers using the capture-derived per-packet layout.

Never force a nominal 48 kHz packet pattern when the device supplies variable implicit feedback. Never fabricate filler packets or silence to satisfy admission.

## Stop, detach, and restart

The safe sequence is:

1. Stop graph production and prevent new ring writes.
2. Stop submitting new transfers.
3. Reap/cancel outstanding transfers and drain completion callbacks.
4. Wake the event thread explicitly; join event and realtime workers.
5. Release claimed interfaces and native driver resources.
6. Close the native driver.
7. Close `UsbDeviceConnection` last.

Apply the same order for permission revocation, detach, USB error, and failed startup cleanup. A restart must not reuse a stale FD or transfer pool. Persist device identity/formats only after a successful probe.

## Failure handling

Classify permission, probe, claim, transfer, metadata-runway, lifecycle, and deadline failures separately. Unknown devices retain conservative generic transfer/watermark policy. The Audient iD4 profile is not a generic USB setting.

## CMake boundary

```cmake
add_subdirectory(path/to/liblowlatencyaudio)
target_link_libraries(your_native_target PRIVATE lowlatencyaudio::lowlatencyaudio)
```

The consumer must not compile a second copy of driver sources or define another libusb target. Android uses the library's bundled pinned libusb USBFS backend; host tests use system libusb.
