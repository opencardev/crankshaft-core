# AA Projection Flicker Plan

## Goal

Stabilize Android Auto projection so it no longer flickers or redraws repeatedly in VNC and physical display modes.

## Current Best Hypothesis

The issue is more likely in the ui-slim frame transport or render path than in HDMI-only output or core decoder renegotiation.

Current indicators:

- Flicker still reproduces in VNC.
- The latest render-side QML change did not stop redraws.
- HDMI is not yet confirmed in the latest report.
- OpenAuto exposes video settings that core currently hardcodes or only partially exposes.
- The current video path is H264 decode -> RGBA -> JPEG base64 over local WebSocket -> QML `Image` data URL, which is a high-churn path.

Latest log evidence:

- Core initializes the decoder once and reaches first frame normally.
- Core reaches `video_first_frame` once and no repeated decoder reinitialization was visible in the sampled logs.
- Core only showed one later display-resolution update from the UI in the sampled window, not a resolution storm.
- ui-slim keeps receiving `android-auto/media/video-frame` events at high cadence.
- ui-slim keeps logging `Video state changed: active` alongside those frame events, which means the UI path still processes per-frame activity even after the last render tweak.

## What We Know

OpenAuto exposes these tunables:

- `Video.FPS`
- `Video.Resolution`
- `Video.ScreenDPI`
- `Video.OMXLayerIndex`
- `Video.MarginWidth`
- `Video.MarginHeight`

Core currently has:

- Partial resolution control through `android-auto/display/resolution`
- Internal FPS state, but no full runtime/config surface
- Hardcoded density and margins in service discovery
- A GStreamer-based decoder path instead of OpenAuto OMX composition

## Shared Working Agreement

Use this document as the single working checklist for both human and AI changes.

Rules:

- Update status inline when a task is completed.
- Keep changes minimal and tied to one hypothesis at a time.
- Prefer one discriminating test before broad refactors.
- Capture what was tried and what the result was.

## Investigation Checklist

- [x] Confirm whether flicker is present in VNC, HDMI, or both.

    VNC is confirmed. HDMI still needs a current same-build confirmation.

- [x] Capture logs for `videoFrameReady`, decoder init, and any frame size changes.
- [x] Check for repeated decoder reinitialization or caps renegotiation.

    Current core logs do not show repeated decoder init or renegotiation.

- [ ] Compare behavior with OpenAuto-like settings: `800x480`, `30 FPS`, `density 140`.
- [ ] Verify whether changes in `resolution`, `fps`, `density`, or `margins` reduce redraw churn.
- [x] Trace the ui-slim handler for `android-auto/media/video-frame` and determine whether it repaints on every event.

    The current path is `CoreClient` -> `AndroidAutoFacade` -> QML `Image` bound to `projectionFrameUrl`.

- [ ] Check whether the UI is receiving duplicate or unchanged frames that still trigger a redraw.
- [ ] Compare frame-event cadence to actual repaint cadence in VNC.
- [ ] Measure whether the redraws track `projectionFrameUrl` updates or only `videoStateChanged(true)` churn.

## Implementation Checklist

- [x] Add core video config surface for AA advertisement values.
- [x] Wire service discovery to use configurable FPS, density, and margins.
- [x] Add websocket FPS control for runtime adjustment.
- [ ] Add optional websocket controls for density and margins if runtime tuning is needed.
- [ ] Add startup telemetry for advertised config, decoder config, and render target.
- [ ] Add temporary ui-slim telemetry for frame cadence, frame size, and repeated-payload detection.
- [ ] Stop treating every video frame as a fresh video-state transition in ui-slim unless the state actually changes.
- [ ] If payloads repeat, coalesce identical frames before updating `projectionFrameUrl`.
- [ ] If payloads differ every frame, replace the data-URL `Image` path with a lower-churn render path.
- [ ] Make sure defaults match the most stable known profile.

## Test Matrix

Run one change at a time and record the effect.

Suggested order:

1. `800x480`, `30 FPS`, `density 140`, zero margins
2. `1280x720`, `30 FPS`, `density 160`, zero margins
3. `1280x720`, `60 FPS`, `density 160`, zero margins
4. `800x480`, `30 FPS`, `density 140`, small non-zero margins

Record for each run:

- Flicker present or not
- Redraw frequency
- Frame size changes in logs
- Decoder reinit events
- Channel reconnects or recoveries

## Discriminating Check

The cheapest check is now to compare frame-event cadence against repaint cadence and payload identity.

Specifically:

- Watch whether `videoFrameReady` logs show changing frame sizes or repeated decoder reinitialization.
- Watch whether ui-slim repaints on every `android-auto/media/video-frame` event even when the frame content and size stay stable.
- Log a cheap frame fingerprint every N frames so we can tell whether redraws come from repeated identical payloads or genuine content changes.

If that appears:

- Prioritize the stream/render-event path in ui-slim.
- Avoid spending more time on HDMI-only investigation until HDMI is confirmed.

If that does not appear:

- Re-check compositor redraws and display scaling behavior.
- Then revisit HDMI and VNC-specific rendering paths.

## Next Steps

1. Instrument ui-slim at the `CoreClient` or `AndroidAutoFacade` frame handoff to log frame count, size, and a cheap payload fingerprint every N frames.
2. Confirm whether `videoStateChanged(true)` is being emitted on every frame and reduce that to edge transitions only.
3. If many consecutive frames are identical, coalesce them before updating `projectionFrameUrl`.
4. If frames genuinely differ every time, plan a transport/render-path change away from JPEG base64 data URLs in QML `Image`.
5. Re-test VNC on the Pi after the instrumentation lands, then re-check HDMI on the same build.

## Notes

- Keep this plan updated as you confirm or reject hypotheses.
- Add timestamps or short outcome notes under each completed item.
- If a change fixes VNC but not HDMI, note that separately.
- Latest Pi evidence on 2026-07-06:

    Core: decoder init once, first frame at `12:24:02`, one later UI resolution update at `12:26:35`.
    ui-slim: repeated `android-auto/media/video-frame` and `Video state changed: active` entries across `12:28:19` to `12:28:27`.
