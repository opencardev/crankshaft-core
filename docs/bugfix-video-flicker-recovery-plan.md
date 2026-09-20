# Android Auto Flicker Fix Plan

## Purpose
Create a targeted recovery fix for Raspberry Pi 3 Android Auto flicker by focusing on USB/AASDK transport instability rather than HDMI mode switching.

## Background
- The collector log shows the Pi is using KMS (`vc4-kms-v3d`) and not legacy `tvservice`.
- HDMI is reported as connected in DRM with `1920x1080` available.
- The kernel log shows a phone USB disconnect/reconnect event for `18d1:2d00`, which strongly indicates transport instability.

## Root cause hypothesis
The flicker is likely caused by transient USB/AASDK transport errors interrupting Android Auto video, not by HDMI display mode changes.

## OpenAuto comparison
OpenAuto appears to keep the display/video surface alive across Android Auto transport issues and does not reconfigure the HDMI/Qt/video pipeline on every reconnect. This suggests the difference is in Crankshaft's recovery path and video decoder/display management, not in the underlying USB/AASDK transport itself.

## Fix goals
- Recover from transient USB/AASDK transport errors gracefully.
- Avoid resetting the display mode while the phone connection is unstable.
- Preserve the existing video decoder/display pipeline state during transport recovery.
- Add logging to correlate transport recovery with USB reconnect events.

## Targeted changes

### 1. Crankshaft-core recovery logic
- Update `src/services/android_auto/RealAndroidAutoService.cpp`:
  - Handle recoverable transport/receive errors in video/audio/control channel handlers. ✓
  - Treat `AASDK::ErrorCode::MESSENGER_INTERTWINED_CHANNELS` as recoverable if it occurs during receive. ✓
  - Add a path to re-arm receive operations after transient errors instead of tearing down the full session. ✓
  - `setDisplayResolution()` now ignores duplicate calls and reinitializes the GStreamer decoder only on genuine resolution changes during an active session. ✓

### 2. Crankshaft-ui-slim — resolution storm fix (new finding)
- Root cause discovered via live Pi logs: `onProjectionFrameChanged` in QML fired on every decoded video frame (~30 fps), calling `updateTouchForwarderDisplaySize()` → `setDisplaySize()` → WebSocket publish of `android-auto/display/resolution` at frame rate.
- This flooded crankshaft-core with `setDisplayResolution()` calls, logging ~16×/second and potentially causing GStreamer notification cascades.
- Fixes in `crankshaft-ui-slim/bugfix/resolution_storm`:
  - Remove `onProjectionFrameChanged` connections from `AAProjectionView.qml` and `main.qml`. ✓
  - Add `m_lastPublishedResolution` deduplication guard in `TouchEventForwarder::setDisplaySize()`. ✓

### 3. AASDK transport behavior
- Ensure AASDK transport layer can distinguish transient USB disconnect/reconnect from fatal session failures.
- Prefer a soft recover/retry path for USB receive errors over a full channel/messenger restart.

### 4. UI display stability
- Confirmed `crankshaft-ui-slim` resolution storm is fixed. ✓
- Keep the existing `1920x1080` display configuration intact while the phone reconnects.

## Logging and diagnostics
- Add logs for:
  - transport error detection
  - receive re-arm/retry attempts
  - transport restart events
  - video state transitions
- Use those logs to confirm recovery timing vs. USB disconnect events.

## Test plan

### Unit tests
- Add coverage for:
  - recoverable transport errors
  - recoverable `MESSENGER_INTERTWINED_CHANNELS` errors
  - no-display-reset behavior during transport recovery

### On-device regression
- Deploy to Raspberry Pi 3 with Android Auto phone.
- Reproduce USB disconnect/reconnect scenarios.
- Verify:
  - display remains at 1920x1080
  - UI/video frame state recovers
  - no HDMI flicker from display re-negotiation

## Deployment checklist
- [x] Implement recovery logic in `RealAndroidAutoService.cpp` (receive re-arm for transient USB errors)
- [x] `setDisplayResolution()` no-op guard + GStreamer reinit on genuine resolution change
- [x] Add `AasdkErrorClassification.h` and unit tests (30 test cases)
- [x] Fix resolution storm in `crankshaft-ui-slim` (QML + C++ deduplication)
- [ ] Add AASDK transport error handling improvements
- [ ] Deploy packages to Pi and verify
- [ ] Test on Raspberry Pi 3 with real phone — confirm flicker stops
- [ ] Confirm flicker stops and recovery is smooth

## Follow-up
If flicker persists after the transport recovery fix:
- Capture deeper DRM/KMS logs around display driver events.
- Check whether the VC4 driver resets or the display pipeline changes during recovery.
- Investigate whether any UI or core component is assuming video stop means display loss.
