# WebRTC Deployment Runbook (Deterministic)

## Purpose

This runbook replaces ad-hoc patch/build/retry cycles with a deterministic workflow.

If runtime prerequisites are missing on target, Crankshaft must explicitly fall back to `websocket-jpeg` with a concrete `video_transport_fallback_reason` rather than repeatedly attempting failing WebRTC setup.

## Definitive Flow

1. Build and deploy one known core revision.
2. Restart services once.
3. Run preflight checks once.
4. Classify result:
   - `preflight-pass`: continue WebRTC validation.
   - `preflight-fail`: stop WebRTC debugging and resolve runtime dependencies.

Preflight now requires a probe pipeline to transition through `PLAYING` and checks for GStreamer bus runtime errors. This catches environments where factories exist but runtime plugins are still incomplete.

## Runtime Preconditions

Target must provide these GStreamer element factories:

- `webrtcbin`
- `rtph264pay`
- `h264parse`
- `appsrc`
- `dtlssrtpenc`
- `dtlssrtpdec`
- `srtpenc`
- `srtpdec`
- `nicesrc`
- `nicesink`

If any are missing, WebRTC bridge initialization is expected to fail.

## One-Shot Validation Commands (Pi)

### 1) Service restart

```bash
sudo systemctl restart crankshaft-core crankshaft-ui-slim
```

### 2) Core log classification

```bash
journalctl --since '10 minutes ago' -u crankshaft-core -o cat \
  | egrep -i 'WebRTC runtime preflight failed|video_transport_fallback_reason|SEARCHING -> CONNECTING|CONNECTING -> CONNECTED|WebRTC bridge error|Falling back to websocket-jpeg'
```

### 3) Factory presence spot-check

```bash
for f in webrtcbin rtph264pay h264parse appsrc dtlssrtpenc dtlssrtpdec srtpenc srtpdec nicesrc nicesink; do
  gst-inspect-1.0 "$f" >/dev/null 2>&1 || echo "missing: $f"
done
```

## Expected Outcomes

### Outcome A: Preflight fail

Core logs contain:

- `WebRTC runtime preflight failed: ...`

Action:

- Treat as environment/package issue, not bridge-linking logic issue.
- Keep transport on `websocket-jpeg` until dependencies are installed.
- Do not iterate on sink pad linking code.

Common signatures under this outcome:

- `probe pipeline runtime error: ... missing a plug-in`
- `probe pipeline failed to enter PLAYING state`

### Outcome B: Preflight pass, bridge fail

Core logs contain:

- `WebRTC bridge error: ...`

Action:

- Investigate signaling/channel sequence with a single captured trace bundle.
- Avoid incremental speculative code edits without new trace evidence.

### Outcome C: WebRTC pass

Core logs show:

- transition past CONNECTING and no WebRTC bridge errors.

Action:

- Lock revision, archive evidence, and stop transport-layer changes.

## Policy to Prevent Retry Loops

- No new WebRTC bridge code changes without one of:
  - preflight pass + new failing trace class not previously addressed, or
  - regression from a known working revision.
- Every transport change must update this runbook if behavior/criteria change.
