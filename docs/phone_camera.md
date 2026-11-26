# Phone camera

The companion Moonlight Android fork can expose a phone camera as **Sunshine
Camera** to Linux meeting, browser and recording apps. The camera is off at the
start of every new stream. Open the in-stream keyboard **⋯** panel, tap **Camera**
and grant camera permission. Choose front/back in the phone's input preferences;
front is the default. Images are not mirrored. Meeting apps can mirror their own
self-view independently.

## Dependencies and setup

Install `v4l2loopback-dkms` and `/usr/bin/ffmpeg` with an H.264 decoder and v4l2
output muxer. The decoder runs in a session-owned external process: Sunshine's
bundled static FFmpeg need not contain the H.264 decoder and no second libavcodec
is linked into the host. The host user needs write access to the video device.

Create a persistent v4l2loopback device with the exact label `Sunshine Camera`,
`exclusive_caps=1` and a free video number. For example, after checking that video42
is free, place this in `/etc/modprobe.d/sunshine-camera.conf`:

```text
options v4l2loopback devices=1 video_nr=42 card_label="Sunshine Camera" exclusive_caps=1
```

Place `v4l2loopback` in `/etc/modules-load.d/sunshine-camera.conf` and load the
module with `modprobe v4l2loopback`. The companion host-configuration repository
provides an idempotent `sunshine/setup-camera.sh` that selects a free number and
waits for the existing udev permissions. It refuses to unload an already loaded
module that lacks the named device. No Sunshine or desktop restart is needed to
create the device. With exclusive capabilities, apps discover capture support
once the phone starts producing frames; enable the phone camera before selecting
it in an app. Sunshine discovers the device through `/sys/class/video4linux/*/name`;
it does not assume a fixed video number.

## Negotiation and encrypted transport

Linux hosts advertise the exact SDP line `a=x-ss-camera.version:1` in DESCRIBE.
The client requires that exact version, a Sunshine peer and negotiated control-v2
AES-GCM encryption. Legacy/official peers receive no camera data. The host drops
cleartext camera packets. No additional port or host-to-client back-channel is
used.

Each type **0x5506** control payload has this 24-byte header, followed by at most
**768** Annex-B H.264 bytes. All integers are little endian.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Version, exactly 1 |
| 1 | 1 | Flags: bit 0 = IDR with SPS/PPS; bit 1 = stop; other bits zero |
| 2 | 2 | Clockwise rotation to upright: 0, 90, 180 or 270 |
| 4 | 4 | Reserved, zero |
| 8 | 4 | Frame sequence modulo 2^32; stop consumes a sequence |
| 12 | 2 | Chunk index, starting at zero |
| 14 | 2 | Chunk count, 1–512 for data; zero for stop |
| 16 | 8 | Capture timestamp in microseconds on the sender clock |
| 24 | 1–768 | Compressed data; absent for stop |

Nonfinal chunks contain exactly 768 bytes. Stop has flags=2, index=count=0 and
exactly 24 bytes. The frame limit is 384 KiB. Channel **0x06** is within the
existing 0x30-channel allocation. Messages use UNSEQUENCED and
UNRELIABLE_FRAGMENT, never reliable fragmentation. With encryption overhead,
768-byte data chunks fit the 900-byte ENet MTU and the 1100-byte encrypted-control
scratch buffer. A runtime bound guards the encrypted send path, including Release
builds.

## Capture, loss and lifecycle

Camera2 feeds a MediaCodec surface at 1280×720, targeting 30 fps and 2 Mbps H.264,
with one-second IDRs, no B-frames where supported and optional low-latency codec
support. The client normalizes AVCC/Annex-B output and includes SPS/PPS before each
IDR. It drops late frames or the rest of a frame after send failure, requests an
IDR, and suppresses dependent frames until that IDR. It never queues a backlog of
encoded frames.

Each host session owns a bounded reassembler and a three-frame worker queue.
Chunks can arrive out of order within one frame. An incomplete frame, missing
sequence, inconsistent metadata or queue overflow freezes the last good image;
dependent frames are discarded until a complete IDR. Old sequences, duplicate
chunks and replayed stops are rejected. One session owns the webcam at a time.
There is no decode, blocking pipe write or video-device I/O in ENet callbacks.

The worker starts `/usr/bin/ffmpeg` with a low-delay, single-threaded H.264 decoder
and a 32-byte probe. It rotates upright, scales and center-crops to fixed
1280×720 YUV420P, then writes to v4l2loopback. It restarts on rotation changes at
an IDR. The `nobuffer` demux option is deliberately omitted: it can discard the
initial probed IDR and delay startup for a full GOP. Pipe writes, frame storage,
queue length and child shutdown waits are bounded.

Camera permission uses a history-retaining activity because Game is noHistory.
Pausing, PiP, backgrounding, stream rotation reconnect and disconnect stop/join
capture before native teardown. Only rotation recreation retains the toggle;
a newly launched stream starts off. Rotation metadata updates while capturing.
A denied permission or capture error turns the toggle off with a retry message.

Off, a one-second input timeout and session teardown close the decoder, write
three black frames, release the device and reap owned children. Closing Sunshine's
CLOEXEC input pipe on host exit also gives the decoder EOF. Missing ffmpeg or a
missing/inaccessible device produces one warning for that receiver and ignores
camera packets. The running host never changes module or device configuration.

## Verification

Unit tests cover framing bounds, replay/stop/wrap behavior, reassembly and
IDR-only recovery. Common-c has opt-in `BUILD_CAMERA_TESTS` Release tests. Android
JVM tests cover framing, codec normalization, orientation and capture gates.

From a scratch directory, run the host tests built in `cmake-build-kwin`:

```sh
/path/to/test_sunshine --gtest_filter='Camera.*:Microphone.*'
SUNSHINE_TEST_CAMERA_INTEGRATION=1 /path/to/test_sunshine --gtest_filter='CameraIntegration.*'
```

The opt-in integration test needs an unused loopback device. It generates an
asymmetric synthetic 720p H.264 clip using system ffmpeg, sends the real chunk
format through the receiver, captures the device through ffmpeg, and verifies
size, 0°/90° orientation, freeze after a lost chunk, IDR recovery, latency and
process cleanup. It never opens a camera or desktop window.

On a physical phone, verify permission grant/denial and retry; toggle on/off;
front/back selection; portrait/landscape upright images; Meet/browser/OBS capture;
background/PiP stop/resume; rotation reconnect and disconnect. Physical camera
quality, OEM codec behavior and meeting-app discovery require that manual check.
