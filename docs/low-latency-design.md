# Low-latency design notes

## Accepted mechanisms

The driver uses lock-free bounded SPSC rings, a preallocated isochronous transfer pool, eventfd wakeups, whole-transfer admission, capture-derived implicit-feedback FIFO layouts, exact rational scheduling, and best-effort urgent scheduling. DirectUsbOutput uses zero-copy two-span PCM conversion into/out of ring reservations.

The two-span path removed staging PCM vectors. Isolated ARM64 measurements reported approximately 8% playback conversion improvement at 32 frames and 6–10% at 256; capture was neutral at 32 and up to about 6% faster at 256. A Xiaomi live profile passed two 30-second cycles at 48 kHz, 24 valid bits in 4-byte subslots, four channels, 48-frame blocks, multiplier 5, with estimated host queue 8.4375 ms. Multiplier 4 and an earlier 32-frame/multiplier-2 profile were not stable in repeated current-load runs. These are measured profiles, not promises.

## Candidates requiring evidence

Transfer batching (one reserve/copy/publish per transfer) and coalesced eventfd notifications (signal insufficient-to-enough transitions) may reduce overhead. Before adoption, test wrap, lost wakes, disconnect, stop/restart, contention, packet status accounting, and FIFO order.

ADPF should receive actual CPU work, excluding USB wait/backpressure. Capacity-aware CPU selection, bounded prefaulting off the RT thread, optional bounded `mlock` under `RLIMIT_MEMLOCK`, and representative PGO are setup/benchmark work, not correctness requirements.

## Rejected approaches

Do not enable global `-ffast-math`, `-Ofast`, `-mcpu=native`, fixed SVE, unverified restrict/alignment assumptions, broad allocator replacement, or `io_uring` as a replacement for USBFS `SUBMITURB`/reap. Android applications cannot assume realtime scheduler, IRQ affinity, cpufreq, usbfs sysctl, or kernel-patch privileges. Validate numerical output, callback p50/p95/p99, deadline misses, xruns, thermal headroom, and power on physical ARM64 before accepting an optimization.
