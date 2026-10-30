# Bluetooth

## 1. Purpose and Scope

- Provide Bluetooth device management (enable/disable, scan, pair, connect, disconnect).
- Connect Bluetooth audio devices. Their outputs are chosen in the core audio output list (see ADR-0005), alongside direct-attached outputs.
- Expose Bluetooth operations/events to UI clients through websocket topics.

Out of scope:

- Detailed UI workflows.
- Vendor-specific Bluetooth stack customization beyond BlueZ/DBus APIs.

## 2. Ownership and Boundaries

- HAL layer: `BluetoothHAL` for BlueZ DBus interactions.
- Manager interface: `BluetoothManager` abstraction, with functional implementation used by `ServiceManager`.
- Audio output: Bluetooth sinks appear in the `AudioRouter` output list (`audio/state`). The Bluetooth layer does not route audio; the user selects the output.
- Event/API bridge: websocket topic handlers under `bluetooth/*`.

## 3. Architecture Summary

Bluetooth support is layered: low-level DBus adapter/device operations in `BluetoothHAL`, and service-friendly operations in `BluetoothManager` implementations. Discovery timeout policy lives in the manager. A connected Bluetooth speaker appears as a PulseAudio sink (`bluez_output.*`), and `AudioRouter` lists it. Audio goes to it only when the user selects it.

## 4. Component Map

| Component | File(s) | Responsibility |
| --- | --- | --- |
| Manager abstraction | `src/hal/wireless/BluetoothManager.h`, `src/hal/wireless/BluetoothManager.cpp` | Interface contract for Bluetooth operations |
| BlueZ HAL | `src/hal/wireless/BluetoothHAL.h`, `src/hal/wireless/BluetoothHAL.cpp` | DBus adapter and device operations |
| Service lifecycle | `src/services/service_manager/ServiceManager.cpp` | Starts/stops Bluetooth manager based on profile or fallback |
| Remote command path | `src/services/websocket/WebSocketServer.cpp` | Handles `bluetooth/*` control topics |
| Output selection | `src/services/audio/AudioRouter.cpp`, `src/services/audio/AudioOutputs.cpp`, `src/hal/multimedia/AudioHAL.cpp` | Lists sinks, applies the chosen output, and reconciles on hot-plug |

## 5. Runtime Sequence

1. `ServiceManager` starts Bluetooth manager when profile includes Bluetooth (or fallback start via websocket request).
2. The manager reads the discovery timeout config.
3. Discovery starts/stops with timeout enforcement.
4. Pair/connect operations are delegated to manager.
5. On successful connect, the device's sink appears in the next audio output listing (within about 5 seconds). Audio is not moved to it automatically.

Shutdown:

- Discovery timers stop and manager/service lifecycles are released during service stop.

## 6. Data and Control Flows

### 6.1 Inbound Inputs

- Websocket topics: `bluetooth/enabled`, `bluetooth/scan/request`, `bluetooth/pair`, `bluetooth/connect`, etc.
- Hardware/DBus events from BlueZ adapter/device changes.
- Config key for discovery timeout.

### 6.2 Processing

- Device discovery results filtered for audio-relevant devices.
- Pair/connect requests executed through manager.
- Output choice is made through the `audio/output/set` topic, not by the Bluetooth layer. The choice is persisted as `core.audio.output`.

### 6.3 Outbound Outputs

- Manager signals (`deviceDiscovered`, `devicePaired`, etc.).
- Audio output events: `audio/state` after every output change, with `outputs`, `output` and `active_output`.
- Websocket result events (`bluetooth/*/result`).
- Structured logs from Bluetooth service and websocket layers.

## 7. Configuration

| Key | Default | Effect | Notes |
| --- | --- | --- | --- |
| `core.bluetooth.discovery_timeout_seconds` | 60 | Auto-stop discovery after timeout | Set to `0` to disable timeout behavior |
| `core.audio.output` | empty | Chosen audio output (empty = system default) | Set with `audio/output/set`; applies to direct and Bluetooth outputs |

## 8. External Dependencies

- BlueZ services on system DBus.
- Qt DBus and Qt Bluetooth support.
- PipeWire or PulseAudio with the `pactl` tool, for listing audio outputs.

Runtime assumptions:

- Bluetooth adapter is present and visible to system DBus.
- Service user has sufficient permissions for Bluetooth operations.

## 9. Observability and Diagnostics

Logs to watch:

- `[AudioRouter] Audio outputs changed: ...`
- `[AudioRouter] Audio output: ...`
- `[AudioRouter] Could not open audio output ...`
- `[WebSocketServer] Bluetooth scan started; timeout in ...`

Checks:

- `journalctl -u crankshaft-core -b | grep -E "AudioRouter|bluetooth|WebSocketServer"`
- `pactl list short sinks` (the sink names that appear in `audio/state`)
- `bluetoothctl show`
- `bluetoothctl devices`

## 10. Failure Modes and Recovery

| Symptom | Likely Cause | Detection | Recovery |
| --- | --- | --- | --- |
| Scan does not return devices | Adapter unavailable or discovery failure | No `deviceDiscovered` events, scan result shows failure | Validate adapter state and DBus availability |
| Pair/connect fails repeatedly | Remote device rejects pairing/profile mismatch | `bluetooth/*/result` success false and service warnings | Remove stale pairing, retry from clean state, verify profiles |
| Speaker connected but audio plays elsewhere | Output not selected, or the sink is not listed | `audio/state` has the `bluez_output.*` sink in `outputs` but `active_output` is empty or different | Select the speaker in the audio output list; check `pactl list short sinks` |

## 11. Security and Safety Notes

- Bluetooth pairing exposes external trust boundary; only trusted user flows should trigger pair/connect.
- Websocket command path must validate payloads and reject malformed addresses.
- Avoid persisting sensitive metadata beyond device identity and route preference.

## 12. Testing Strategy

- Unit tests for discovery timeout behavior and preference persistence logic.
- Integration tests with BlueZ test doubles or controlled adapter environment.
- Manual tests for pair/connect/disconnect plus audio-route handoff.

Manual checklist:

1. Enable Bluetooth and confirm adapter state transitions.
2. Run scan and verify timeout behavior.
3. Pair/connect known audio device.
4. Verify audio route update and persistence after restart.

## 13. Known Gaps and Follow-ups

- Routing to a Bluetooth output automatically when it connects is not implemented. The choice is manual (ADR-0005).
- Add explicit profile-level validation for A2DP/HFP route compatibility before routing.
- Add richer telemetry for pairing/connection failure categories.

## 14. Change Log Notes

- 2026-06: Added comprehensive Bluetooth architecture documentation with service, manager, HAL, and websocket control-path coverage.
- 2026-10: Removed `BluetoothService`, which was never constructed. Output selection moved to `AudioRouter` (ADR-0005).
