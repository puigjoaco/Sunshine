# Phone microphone extension

Linux builds with PipeWire capture support advertise `a=x-ss-microphone.version:1`
in RTSP DESCRIBE. Clients must negotiate that exact version and control-v2
AES-GCM before sending audio. Hosts and clients without this attribute continue
the existing protocol. The feature uses the existing encrypted ENet control
socket and adds no listening port.

Control type `0x5505`, channel `0x06`, carries unreliable, unsequenced messages.
The encrypted payload is a 16-byte header followed by one raw Opus packet
(maximum 1024 bytes). All integers are little endian:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Version, 1 |
| 1 | 1 | 1 = audio, 0 = stop/mute |
| 2 | 2 | Samples per frame, 960 |
| 4 | 4 | Packet sequence, modulo 2^32 |
| 8 | 8 | Capture timestamp, 48 kHz sample clock |
| 16 | 0–1024 | Mono 48 kHz Opus, exactly 20 ms; empty only for stop |

The sender increments the sequence for both audio and stop. The host rejects
old/duplicate frames and reordered stops, and compares capture timestamp deltas
without requiring synchronized clocks. A 40–80 ms adaptive reorder buffer uses
Opus PLC for missing frames and bounds pending audio to eight packets. The PCM
ring holds at most 40 ms and evicts a complete 20 ms frame on overrun to leave
headroom for graph quanta, rather than repeatedly clipping small sample ranges.
A 500 ms input timeout covers lost stop packets and client crashes. Decoding and PipeWire/Pulse work run outside the ENet thread.

The native `PW_DIRECTION_OUTPUT` source has `media.class=Audio/Source`, stable
name `sunshine_microphone`, and description **Sunshine Microphone**. One session
at a time owns its audio. After its first use, the node remains silent across
mute, pause, and reconnect so recording applications keep their existing links.
It disappears when Sunshine exits. No previous speech remains queued on stop.

An independent, unprivileged guardian uses PulseAudio's default-source API,
mirroring the existing default-sink switch. It remembers the previous default
in memory, selects the phone source for that streaming session, and restores the
previous source when its parent lifetime pipe closes, including after a host
crash. Muting keeps the selected phone source silent until unmute or disconnect.
An explicit user selection while streaming is respected. No account details or
source names are written to a state file.

The Android sender uses `VOICE_RECOGNITION`, requests platform AEC/NS where
available, and uses MediaCodec Opus at 40 kbps VBR. Recognition was selected
after device probes initialized both sources and effects successfully but
communication returned silence. For negotiated microphone sessions, the client
disables ENet's RTT-based
unreliable packet deceleration. At its minimum, that throttle can silently discard
31 of 32 frames locally even when `enet_peer_send()` succeeds, leaving 640 ms
between 20 ms voice packets. Audio stays unreliable and unsequenced; no queued
speech is retransmitted. AudioRecord starts before codec initialization so hardware
warm-up overlaps encoder setup on unmute.

Encoder availability is checked before capture;
an unavailable Android 10+ Opus encoder produces a clear message.

Focused tests:

```sh
cmake --build cmake-build-kwin --target test_sunshine
cmake-build-kwin/tests/test_sunshine --gtest_filter='Microphone.*'
SUNSHINE_TEST_MICROPHONE_INTEGRATION=1 cmake-build-kwin/tests/test_sunshine \
  --gtest_filter='MicrophoneIntegration.*'
```

The opt-in integration test temporarily changes the desktop default microphone,
feeds synthetic Opus, records the native source with `pw-record`, checks a 733 Hz
tone (which exposes whole-frame skips), measures packet-to-record latency, and
checks recording continuity across a receiver reconnect. Run it only in an idle local PipeWire session. It requires
`pw-record` in PATH and does not record a physical microphone.
