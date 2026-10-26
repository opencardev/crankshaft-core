# Audio Routing

## 1. Purpose and Scope

- Provide robust Android Auto audio routing from decoded AA audio streams to local outputs.
- Coordinate backend availability detection (PipeWire/PulseAudio) and stream-role routing.
- Integrate with MediaPipeline and AudioHAL for actual PCM playback.

Out of scope:

- Full-featured policy engine for per-app routing.
- Device-specific DSP tuning and equalization.

## 2. Ownership and Boundaries

- Primary modules: `AudioRouter`, `MediaPipeline`, `AudioHAL`.
- External runtime: PipeWire, pipewire-pulse compatibility socket, PulseAudio tooling fallback.
- Consumers: `RealAndroidAutoService`, Bluetooth routing flows, UI-level volume abstractions.

## 3. Architecture Summary

`AudioRouter` is the AA-oriented routing coordinator. It accepts audio buffers tagged by stream role, applies lightweight ducking policy, and forwards PCM into `MediaPipeline`. `MediaPipeline` owns the active audio/video HALs and pushes data into `AudioHAL`, which is implemented as a GStreamer pipeline with dynamic sink selection.

## 4. Component Map

| Component | File(s) | Responsibility |
| --- | --- | --- |
| Audio routing coordinator | `src/services/audio/AudioRouter.h`, `src/services/audio/AudioRouter.cpp` | Backend detection, stream role mapping, ducking, routing decisions |
| Media orchestration | `src/hal/multimedia/MediaPipeline.h`, `src/hal/multimedia/MediaPipeline.cpp` | Owns audio/video HAL lifecycle and data push entry points |
| Audio HAL | `src/hal/multimedia/AudioHAL.h`, `src/hal/multimedia/AudioHAL.cpp` | GStreamer pipeline setup and sink route changes |
| AA integration point | `src/services/android_auto/RealAndroidAutoService.cpp` | Creates and initializes `AudioRouter` |

## 5. Runtime Sequence

1. `ServiceManager` starts Android Auto and media pipeline setup.
2. `RealAndroidAutoService` creates `AudioRouter` and calls `initialize()`.
3. `AudioRouter` detects PipeWire first, then PulseAudio fallback.
4. Audio frames from AA channels call `routeAudioFrame(...)` and are forwarded to `MediaPipeline::pushAudioData(...)`.
5. `AudioHAL` pushes data into GStreamer and sink output.

Shutdown:

- Service teardown stops Android Auto and media pipeline; `AudioRouter` shuts down with service destruction.

## 6. Data and Control Flows

### 6.1 Inbound Inputs

- Decoded PCM data from AA audio channels.
- Stream role (`MEDIA`, `SYSTEM_AUDIO`, `GUIDANCE`, etc.).
- Runtime environment (`XDG_RUNTIME_DIR`, `PIPEWIRE_RUNTIME_DIR`, `PULSE_SERVER`).

### 6.2 Processing

- Detect backend using runtime sockets first (`/run/user/<uid>/pipewire-0`, pulse socket), then command probes (`pw-cli`, `pactl`).
- Apply ducking for non-guidance streams when guidance priority is active.
- Resolve target output device and route via `MediaPipeline`.

### 6.3 Outbound Outputs

- Structured logs (`[AudioRouter] ...`).
- PCM data pushed into media pipeline.
- Device selection and stream config updates.

## 7. Configuration

| Key | Default | Effect | Notes |
| --- | --- | --- | --- |
| `XDG_RUNTIME_DIR` | derived from service env | Runtime socket root for PipeWire/Pulse | Also inferred from effective UID fallback |
| `PIPEWIRE_RUNTIME_DIR` | derived from service env | Preferred PipeWire runtime path | Falls back to `XDG_RUNTIME_DIR` candidate list |
| `PULSE_SERVER` | `unix:/run/user/%U/pulse/native` in service env | Pulse native socket endpoint | Used for pulse socket detection and pactl environment |

## 8. External Dependencies

- GStreamer runtime for `AudioHAL` pipeline.
- PipeWire (`pipewire`, `pipewire-bin`, `pipewire-pulse`) and `wireplumber` in target images.
- Optional CLI probes: `pw-cli`, `pactl` (fallback checks).

Required service/runtime assumptions:

- `crankshaft-core` runs with writable HOME/XDG dirs under `/var/lib/crankshaft`.
- Runtime user sockets available under `/run/user/<uid>`.

## 9. Observability and Diagnostics

Logs to watch:

- `[AudioRouter] Initialising audio router`
- `[AudioRouter] PipeWire backend initialised`
- `[AudioRouter] PulseAudio backend initialised`
- `[AudioRouter] No audio backend available`

Checks:

- `systemctl status crankshaft-core`
- `journalctl -u crankshaft-core -b | grep -E "AudioRouter|AudioHAL|PipeWire|Pulse"`
- `systemctl show crankshaft-core -p Environment`

## 10. Failure Modes and Recovery

| Symptom | Likely Cause | Detection | Recovery |
| --- | --- | --- | --- |
| No audio backend available | Missing runtime env or user socket path | Core logs show backend probe failures | Verify service env, PipeWire user units, linger, runtime sockets |
| 0 audio output devices | Qt multimedia device discovery unavailable | `Found 0 audio output devices` log | Verify audio stack startup and permission paths |
| Audio push failures | MediaPipeline inactive or stream not enabled | `Failed to push audio data to pipeline` | Validate pipeline startup order and AA channel state |

## 11. Security and Safety Notes

- Service runs as `crankshaft` (non-root) with hardened systemd unit.
- Writable paths should be restricted to known runtime directories.
- Audio data is treated as untrusted stream input; failure should degrade gracefully.

## 12. Testing Strategy

- Unit test backend detection branches with controlled env and fake socket presence.
- Integration test with PipeWire user services active and AA media stream.
- Regression test for startup with no CLI tools but valid sockets.

Manual checklist:

1. Confirm core service environment values.
2. Confirm socket files exist.
3. Start projection and confirm audio playback.
4. Restart service and verify no `/nonexistent` path usage.

## 13. Known Gaps and Follow-ups

- Router-level ALSA fallback is documented but not explicitly implemented in backend init path.
- Output selection is hardware-unverified: `pulsesink` with a named sink, and hot-plug reconciliation (ADR-0005).
- Add explicit metrics for backend detection outcomes.

## 14. Change Log Notes

- 2026-10: Output selection moved to PulseAudio sinks (`pactl`), with the choice applied to `pulsesink` and reconciled on hot-plug (ADR-0005). `AudioRouter::setAudioDevice` and the `AudioHAL` route enum were removed.
- 2026-06: Added robust socket-first backend detection and runtime-dir candidate fallback to reduce false negatives in systemd service context.
