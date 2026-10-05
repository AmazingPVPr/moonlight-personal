# Video pause and startup warmup checks

Run `bash tests/video-pause/run.sh` from the repository root. It exercises the
production video admission state and Pacer queues/render thread without a host
or display. An injected monotonic clock makes the startup deadline deterministic.
The checks cover multiple frames during warmup, expiry at exactly one second,
expiry without a new frame, queue draining, keyframe recovery, clock wrap,
stale presentation generations, bounded failed previews, retained hardware
surface ownership, uninterrupted foreground playback, and independent audio
and video countdowns.

Run `bash tests/video-pause/run-startup-vulkan.sh` with access to a local X11 or
XWayland display for the production Vulkan renderer check. A hidden window
receives several synthetic video images through Pacer during the injected
one-second warmup. The check confirms the initial black mapping buffer does not
start that timer, then confirms presentation and admission stop at its deadline.
No host is contacted and the test window does not take focus. Exit status 77
means that the display or Vulkan renderer was unavailable.

Production clocks use Limelight's monotonic microsecond clock converted to
32-bit milliseconds. Warmup starts at the first successfully presented stream
image, lasts 1000 ms, and does not extend when later images arrive. Before that
first presentation, an admitted keyframe arms a 2000 ms failure deadline; later
packets do not reset it. A failed paused preview waits for an actual focus or
visibility improvement before retrying. Session ticks and caps its event wait
at the same deadlines, so host stalls cannot keep decoding enabled indefinitely.

The requested background pause remains set during warmup. Once its budget ends,
incoming video is drained without decoding; the network session remains active.
Resuming invalidates stale references and requests a fresh keyframe. Completing
warmup during normal playback does not flush the codec or interrupt playback.
