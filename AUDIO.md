<!-- Modified for Goblin Skiff on 2026-09-19. -->
# Session audio

Status: design requirements, not an implemented audio bridge. The current control
popup is still a directory browser. No microphone capture or codec transport is
enabled by this document.

## Devices and routing

The local client uses native audio APIs for microphone capture and headphone or
speaker playback. The remote bridge creates per-session PulseAudio endpoints
(including compatibility with PipeWire's PulseAudio service):

```
PULSE_SOURCE=goblin_mic_SESSION_ID
PULSE_SINK=goblin_speaker_SESSION_ID
```

Incoming microphone samples feed the virtual recording source. Application
output to the virtual sink is captured for delivery to the local speakers.
Export these variables only after the endpoints exist. Do not change the audio
server's global default devices. Keep the endpoints and their names alive
across network interruptions, roaming, stream reconfiguration and audio mute.
Remove only this session's endpoints when the session ends.

Backend inspection:

- `../gwbasic/src/sound.rs` uses CPAL 0.15's default output device. The locally
  available CPAL 0.15.3 implementation chooses ALSA as its Linux default host.
  Merely exporting PulseAudio device variables does not redirect a direct ALSA
  hardware device. Provide session-scoped ALSA-to-Pulse routing for the default
  PCM without overwriting `~/.asoundrc` or system configuration.
- The installed macOS Grok 1.0.13 binary contains CPAL 0.15.3 source paths and
  links CoreAudio/AudioUnit. A Linux Grok 1.0.13 executable exists on naamah, but
  its audio backend has not yet been established from source or a runtime trace.
  Do not claim end-to-end Grok compatibility until it has been tested.
- Naamah's PipeWire PulseAudio service is running, and runtime libpulse/libopus
  libraries exist; their development packages were not found by pkg-config.
  The bridge has not been installed or tested against that service.

## Control-panel audio page (Skiff prefix, then 0)

Keep controls, preferences and pending negotiations for the life of the Skiff
session, independently of whether the popup is visible. Opening the audio page
does not itself authorize microphone capture. The terminal behind it stays live.

- Master audio on/off, plus independent microphone and speaker enable/mute.
- Independent outgoing (microphone to remote) and incoming (remote to speaker)
  codec and target payload bitrate. Offer only mutually supported configurations.
- Microphone gain and playback volume, scoped to this session rather than
  changing system-wide volume.
- Local microphone level bar, peak/clipping indication and explicit microphone
  test. Testing must not transmit the test capture to the remote host while the
  microphone uplink is disabled. Stop test capture when the user ends the test
  or hides the popup, unless the microphone is already enabled for transmission.
- An explicit short, conservative-volume headphone test; distinguish this local
  device test from an eventual end-to-end remote loopback test.
- Active/pending/unsupported/failed configuration, mute state, packet loss,
  discarded/late frames and current traffic. Distinguish codec payload bitrate
  from actual wire traffic, which includes framing, encryption and IP/UDP.

Master off releases local capture, stops transmission and playback, and clears
queued media. Turning audio back on starts fresh stream generations. Reopening
the menu must not unmute audio or replay a previous headphone test.

## Transport and codecs

Carry live media in separate logical channels inside the authenticated Skiff
connection, outside terminal screen-state synchronization. Prioritize live
audio alongside interactive traffic and above forwarded socket/bulk payloads,
with bounded work and queues so it cannot monopolize keyboard/screen service.

Use reliable, bounded stream-control messages for capability negotiation,
enable/disable, configuration changes and errors. Each direction has its own
generation, sequence numbers and media timestamps. A codec/rate change must
be acknowledged before using the new format; discard old-generation media
instead of feeding it to a new decoder. Failure leaves a clear disabled or
previously agreed state, not a silent codec substitution.

Bound capture, transport, jitter and playback queues by both bytes and audio
duration. Discard frames that miss playout deadlines. On reconnect, flush stale
media and codec prediction state, negotiate fresh generations, and retain the
virtual endpoints so applications do not lose their devices. Do not retransmit
expired live audio as if it were file data.

Requested codec profiles, subject to pinned source/model/dependency review:

| Codec | Payload bitrate | Intended use |
| --- | --- | --- |
| Codec 2 | 700 / 1,300 bit/s | Very constrained speech; test recognition quality |
| LPCNet | 1,600 bit/s | Experimental neural speech; CPU and model compatibility tests |
| Lyra v2 | 3.2 / 6 / 9.2 kbit/s | Neural speech with several kbps available |
| Opus | 6 kbit/s and up | Speech initially 12–24 kbps; stereo 64–128 kbps |

Low-rate modes need deliberate packetization: codec bitrate alone is not a
prediction of satellite-link consumption. Silence suppression, packet overhead,
latency, loss handling and the opposite direction's traffic must be measured.
Do not send continuous full-rate media packets for idle/silent audio.

## Acceptance checks

Use generated samples by default, not the user's microphone. Test bidirectional
audio, independent codec/rate changes under loss, reordering/duplication, stale
frames after reconnect, audio on/off, gain/volume bounds, meter behavior and
headphone-test cancellation. Verify the endpoints retain their names and handles,
and keyboard/screen traffic stays responsive with blocked audio devices or
expensive codecs. End-to-end Grok and BASIC routing remains a separate required
integration check.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the license and source-distribution plan.
