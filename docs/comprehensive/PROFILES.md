# Profiles

## 1. Purpose

Profiles control which services are started and how device-specific settings are passed into those services at runtime.

In practice, Android Auto runtime behavior (including video transport mode) is driven by profile device settings.

## 2. Where Profiles Are Loaded From

At startup, core initializes `ProfileManager` using:

- config key: `core.profile.configDir`
- default: `/etc/crankshaft/profiles`

Reference:

- `src/CoreApplicationRunner.cpp` (`ProfileManager profileConfigDir` setup)

If profile files are missing in that directory, `ProfileManager` creates default profiles and saves them there.

## 3. Profile Files

`ProfileManager` reads/writes two JSON files:

- `host_profiles.json`
- `vehicle_profiles.json`

These are stored under the active profile directory (`core.profile.configDir`).

## 4. How Profiles Affect Service Startup

`ServiceManager` reads the active host profile and iterates configured devices.

For the `AndroidAuto` device:

1. It starts mock vs real service based on `useMock`.
2. It calls `configureTransport(device.settings)` on the Android Auto service.

Reference:

- `src/services/service_manager/ServiceManager.cpp`

This means `device.settings` has higher practical impact for Android Auto transport behavior than global assumptions in `/etc/crankshaft/crankshaft.json`.

## 5. Android Auto Transport Keys

`RealAndroidAutoService::configureTransport()` resolves transport mode from settings in this order:

1. `video.transport_mode`
2. `android_auto.video_transport_mode`
3. fallback default in code

Reference:

- `src/services/android_auto/RealAndroidAutoService.cpp`

Accepted values:

- `webrtc`
- `websocket-jpeg`

Unknown values are treated as `websocket-jpeg`.

## 6. Operational Notes

- Editing `/etc/crankshaft/crankshaft.json` alone does not guarantee Android Auto transport changes if profile `device.settings` already set a transport key.
- To verify live mode, check core logs for:
  - `Configured video transport mode: ...`
  - `video_transport_mode=...` in `channel-status` entries.
- If runtime mode is unexpected, inspect the active host profile JSON under `core.profile.configDir` first.

## 7. Quick Verification Commands

```bash
# Show profile config directory in effect (if custom key is set)
# otherwise default is /etc/crankshaft/profiles

# Show active profile files
sudo ls -la /etc/crankshaft/profiles
sudo cat /etc/crankshaft/profiles/host_profiles.json

# Check selected transport mode in logs
journalctl -u crankshaft-core -n 200 --no-pager \
  | grep -E "Configured video transport mode|video_transport_mode="
```
