/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2025 OpenCarDev Team
 *
 *  Crankshaft is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Crankshaft is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Crankshaft. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QList>
#include <QSet>
#include <QThread>
class QTimer;
#include <boost/asio.hpp>
#include <memory>

#include "../../hal/multimedia/IAudioMixer.h"
#include "../../hal/multimedia/IVideoDecoder.h"
#include "AndroidAutoService.h"

// Forward declarations
class SessionStore;
class EventBus;
class AudioRouter;
struct libusb_context;
class AAControlEventHandler;
class AAVideoEventHandler;
class AAAudioEventHandler;
class AAInputEventHandler;
class AASensorEventHandler;
class AAMicrophoneEventHandler;
class AABluetoothEventHandler;
class AAWifiProjectionEventHandler;
class IWirelessNetworkManager;

// Forward declarations for AASDK
#ifdef CRANKSHAFT_AASDK_OLD_API
#include <aasdk/USB/IAOAPDevice.hpp>
#endif
namespace aasdk {
namespace usb {
#ifndef CRANKSHAFT_AASDK_OLD_API
class IAOAPDevice;
#endif
class IUSBWrapper;
class IUSBHub;
class IAccessoryModeQueryFactory;
class IAccessoryModeQueryChainFactory;
class IAccessoryModeQueryChain;
}  // namespace usb
namespace messenger {
class IMessenger;
class ICryptor;
}  // namespace messenger
namespace transport {
class ITransport;
}  // namespace transport
namespace tcp {
class TCPWrapper;
}  // namespace tcp
namespace channel {
namespace mediasource {
class IMediaSourceServiceEventHandler;
namespace audio {
class MicrophoneAudioChannel;
}  // namespace audio
}  // namespace mediasource
namespace mediasink {
namespace video {
class IVideoMediaSinkServiceEventHandler;
namespace channel {
class VideoChannel;
}  // namespace channel
}  // namespace video
namespace audio {
class IAudioMediaSinkServiceEventHandler;
namespace channel {
class MediaAudioChannel;
class SystemAudioChannel;
class GuidanceAudioChannel;
class TelephonyAudioChannel;
}  // namespace channel
}  // namespace audio
}  // namespace mediasink
namespace inputsource {
class IInputSourceServiceEventHandler;
class InputSourceService;
}  // namespace inputsource
namespace sensorsource {
class ISensorSourceServiceEventHandler;
class SensorSourceService;
}  // namespace sensorsource
namespace bluetooth {
class IBluetoothServiceEventHandler;
class BluetoothService;
}  // namespace bluetooth
namespace wifiprojection {
class IWifiProjectionServiceEventHandler;
class IWifiProjectionService;
}  // namespace wifiprojection
namespace control {
class IControlServiceChannelEventHandler;
class ControlServiceChannel;
}  // namespace control
}  // namespace channel
}  // namespace aasdk

// Boost.Asio types included via headers

class MediaPipeline;

/**
 * @brief Real Android Auto service implementation using AASDK
 *
 * Implements full Android Auto protocol using AASDK library.
 * Handles USB device detection, AOAP protocol, and media streaming.
 */
class RealAndroidAutoService : public AndroidAutoService {
  Q_OBJECT

 public:
  // Session state matches data-model.md Session entity states
  enum class SessionState {
    NEGOTIATING,  // Handshake in progress
    ACTIVE,       // Connection established and projecting
    SUSPENDED,    // Temporarily paused (network drop, user switch)
    ENDED,        // Cleanly disconnected
    ERROR         // Fatal error occurred
  };

  explicit RealAndroidAutoService(MediaPipeline* mediaPipeline, QObject* parent = nullptr);
  ~RealAndroidAutoService() override;

  void configureTransport(const QMap<QString, QVariant>& settings) override;
  void setWirelessNetworkManager(
      const std::shared_ptr<IWirelessNetworkManager>& manager) override;
  void setEventBus(EventBus* eventBus) {
    m_eventBus = eventBus;
  }

  bool initialise() override;
  void deinitialise() override;

  ConnectionState getConnectionState() const override {
    return m_state;
  }
  bool isConnected() const override {
    return m_state == ConnectionState::CONNECTED;
  }
  AndroidDevice getConnectedDevice() const override {
    return m_device;
  }

  bool startSearching() override;
  void stopSearching() override;
  bool connectToDevice(const QString& serial) override;
  bool disconnect() override;

  bool setDisplayResolution(const QSize& resolution) override;
  QSize getDisplayResolution() const override {
    return m_resolution;
  }

  bool setFramerate(int fps) override;
  int getFramerate() const override {
    return m_fps;
  }

  bool sendTouchInput(int x, int y, int action) override;
  bool sendKeyInput(int key_code, int action) override;

  bool requestAudioFocus() override;
  bool abandonAudioFocus() override;

  int getFrameDropCount() const override {
    return m_droppedFrames;
  }
  int getLatency() const override {
    return m_latency;
  }

  bool setAudioEnabled(bool enabled) override;
  QJsonObject getAudioConfig() const override;

  // Channel configuration
  struct ChannelConfig {
    bool videoEnabled{true};
    bool mediaAudioEnabled{true};
    bool systemAudioEnabled{true};
    bool speechAudioEnabled{true};
    bool telephonyAudioEnabled{false};
    bool microphoneEnabled{true};
    bool inputEnabled{true};
    bool sensorEnabled{true};
    bool bluetoothEnabled{false};
  };

  void setChannelConfig(const ChannelConfig& config);
  ChannelConfig getChannelConfig() const {
    return m_channelConfig;
  }

 signals:
  /**
   * @brief Emitted when AA session state changes
   */
  void sessionStateChanged(const QString& sessionId, const QString& state);

 private:
  friend class AAControlEventHandler;
  friend class AAVideoEventHandler;
  friend class AAAudioEventHandler;
  friend class AAMicrophoneEventHandler;
  friend class AAInputEventHandler;
  friend class AASensorEventHandler;
  friend class AABluetoothEventHandler;
  friend class AAWifiProjectionEventHandler;

  void setupAASDK();
  void cleanupAASDK();
  void setupChannels();
  void setupChannelsWithTransport();
  void cleanupChannels();
  void handleDeviceDetected();
  void handleDeviceRemoved();
  void handleConnectionEstablished();
  void handleConnectionLost();
  void updateStats();
  void transitionToState(ConnectionState newState);
  void startUSBHubDetection();
  [[nodiscard]] auto extractDeviceSerialFromUSB() -> QString;

  // Session state management
  void transitionToSessionState(SessionState newState);
  auto sessionStateToString(SessionState state) const -> QString;
  void createSessionForDevice(const QString& deviceId);
  void endCurrentSession();
  void updateSessionHeartbeat();

  // Channel event handlers
  void onVideoChannelUpdate(const QByteArray& data, int width, int height);
  void onMediaAudioChannelUpdate(const QByteArray& data);
  void onSystemAudioChannelUpdate(const QByteArray& data);
  void onSpeechAudioChannelUpdate(const QByteArray& data);
  void onSensorRequest();
  void onBluetoothPairingRequest(const QString& deviceName);
  void onChannelError(const QString& channelName, const QString& error);
  void resetProjectionStatus(const QString& reason);
  void publishProjectionStatus(const QString& reason);
  void ensureProjectionIdleWatchdogTimer();
  void updateProjectionIdleWatchdog(const QString& reason);
  void stopProjectionIdleWatchdog(const QString& reason);
  void startControlPingLoop();
  void stopControlPingLoop();
  void sendControlPingRequest();
  [[nodiscard]] auto isProjectionReady() const -> bool;
  [[nodiscard]] auto isMediaAudioReady() const -> bool;
  void traceControlEvent(const QString& event, const QString& details = QString());
  void resetControlTrace(const QString& reason);
  void dumpControlTrace(const QString& reason) const;
  void traceServiceDiscoveryResponse() const;
  void traceChannelReceiveArm(const QString& channelName, const QString& reason);

  // Audio routing for AA channels
  void routeMediaAudioToVehicle(const QByteArray& audioData);
  void routeGuidanceAudioToVehicle(const QByteArray& audioData);
  void routeSystemAudioToVehicle(const QByteArray& audioData);

  // AASDK callbacks
  void onVideoFrame(const uint8_t* data, int size, int width, int height);
  void onAudioData(const QByteArray& data);
  void onUSBHotplug(bool connected);
  void checkForConnectedDevices();  // Fallback device detection
  void ensureAoapRetryResetTimer();
  void armAoapRetryResetWindowIfNeeded();
  void sendControlVersionRequest();
  void performImmediateTransportRecovery(const QString& reason);
  void scheduleVideoFocusKickAfterServiceDiscovery();
  void armNonControlReceivesAfterControlReady();
  void armDeferredChannelReceivesAfterServiceDiscovery();
  void armOptionalChannelReceivesAfterPrimaryStart();

  // Transport mode configuration
  enum class TransportMode { Auto, USB, Wireless };
  TransportMode getTransportMode() const;
  auto setupUSBTransport() -> bool;
  auto setupTCPTransport(const QString& host, quint16 port) -> bool;
  auto setupTCPServerTransport(quint16 port) -> bool;

  ConnectionState m_state{ConnectionState::DISCONNECTED};
  AndroidDevice m_device;
  QSize m_resolution{1024, 600};
  int m_fps{30};
  bool m_audioEnabled{true};
  ChannelConfig m_channelConfig;

  // Session state tracking
  SessionState m_sessionState{SessionState::ENDED};
  QString m_currentSessionId;
  QString m_currentDeviceId;
  SessionStore* m_sessionStore{nullptr};
  QTimer* m_heartbeatTimer{nullptr};
  EventBus* m_eventBus{nullptr};
  AudioRouter* m_audioRouter{nullptr};

  // Statistics
  int m_droppedFrames{0};
  int m_latency{0};
  quint64 m_videoChannelUpdateCount{0};
  quint64 m_videoPayloadCount{0};
  quint64 m_videoDecodeSubmitCount{0};
  quint64 m_videoDecodeRejectCount{0};
  quint64 m_videoDecodedFrameCount{0};
  quint64 m_mediaAudioUpdateCount{0};
  quint64 m_mediaAudioPayloadCount{0};
  quint64 m_mediaAudioMixCount{0};
  quint64 m_systemAudioUpdateCount{0};
  quint64 m_speechAudioUpdateCount{0};
  quint64 m_sensorRequestCount{0};
  quint64 m_bluetoothPairingRequestCount{0};
  qint64 m_lastVideoChannelDebugMs{0};
  qint64 m_lastVideoDecodeDebugMs{0};
  qint64 m_lastMediaAudioDebugMs{0};
  qint64 m_lastSystemAudioDebugMs{0};
  qint64 m_lastSpeechAudioDebugMs{0};
  qint64 m_lastSensorDebugMs{0};
  qint64 m_lastBluetoothDebugMs{0};
  bool m_deviceGoneRecoveryScheduled{false};

  // Projection channel readiness tracking
  bool m_controlVersionReceived{false};
  bool m_serviceDiscoveryCompleted{false};
  bool m_videoChannelOpened{false};
  bool m_videoConfigured{false};
  bool m_videoStarted{false};
  bool m_videoFrameReceived{false};
  bool m_mediaAudioConfigured{false};
  bool m_mediaAudioStarted{false};
  bool m_mediaAudioFrameReceived{false};
  bool m_nonControlReceivesArmed{false};
  bool m_optionalChannelsArmed{false};
  bool m_videoPayloadSeen{false};
  bool m_audioPayloadSeen{false};
  bool m_lastProjectionReady{false};
  bool m_aasdkTeardownInProgress{false};
  QTimer* m_projectionIdleWatchdogTimer{nullptr};
  qint64 m_projectionIdleWatchdogStartedMs{0};
  int m_projectionIdleWatchdogTickCount{0};
  QString m_projectionIdleWatchdogLastReason;
  bool m_projectionIdleRecoveryScheduled{false};
  int m_projectionIdleRecoveryCount{0};
  int m_projectionStreamNudgeCount{0};
  qint64 m_projectionStreamLastNudgeMs{0};
  // Epoch counter increments for each logical stream-nudge attempt
  int m_projectionStreamNudgeEpoch{0};
  // Last epoch that was counted towards the pre-start consecutive timeout counter
  int m_preStartNudgeLastCountedEpoch{0};
  int m_preStartNudgeNative2ConsecutiveTimeouts{0};
  int m_postDiscoveryNoDeviceGraceRecoveryCount{0};
  QTimer* m_controlPingTimer{nullptr};
  QSet<QString> m_channelReceiveArmTraceKeys;

  // AASDK components
  MediaPipeline* m_mediaPipeline{nullptr};
  // Use macro to allow switching between old and new aasdk
#ifdef CRANKSHAFT_AASDK_OLD_API
  std::unique_ptr<boost::asio::io_service> m_ioService;
#else
  std::shared_ptr<boost::asio::io_service> m_ioService;
#endif
  std::unique_ptr<QThread> m_aasdkThread;
  QTimer* m_ioServiceTimer{nullptr};
  QTimer* m_deviceDetectionTimer{nullptr};  // Fallback device detection timer
  QTimer* m_slowdownTimer{nullptr};         // Separate timer for slowdown logic

  // Transport configuration
  TransportMode m_transportMode{TransportMode::Auto};
  QString m_wirelessHost;
  quint16 m_wirelessPort{5277};
  bool m_wirelessEnabled{false};
  bool m_wirelessHotspotAutoStart{false};
  QString m_wirelessHotspotSsid;
  QString m_wirelessHotspotPassword;
  int m_wirelessHotspotChannel{0};
  QString m_wirelessHotspotBssid;
  std::shared_ptr<IWirelessNetworkManager> m_wirelessNetworkManager;

  // Strands for channel operations
  std::unique_ptr<boost::asio::io_service::strand> m_strand;

  // AOAP negotiation state
  bool m_aoapInProgress{false};
  int m_aoapAttempts{0};
  QTimer* m_aoapRetryResetTimer{nullptr};
  bool m_usbHubDetectionStarted{false};
  bool m_initialScanTriggered{false};
  quint64 m_usbSearchGeneration{0};
  int m_failedDeviceCheckCount{0};  // Track failed checks for slowdown (issue #83)
  static constexpr int m_aoapMaxAttempts = 3;
  static constexpr int m_aoapResetMs = 5 * 60 * 1000;  // 5 minutes

  // Pointers to AASDK objects (owned by io_service)
  std::shared_ptr<aasdk::usb::IUSBWrapper> m_usbWrapper;
  std::shared_ptr<aasdk::usb::IAccessoryModeQueryFactory> m_queryFactory;
  std::shared_ptr<aasdk::usb::IAccessoryModeQueryChainFactory> m_queryChainFactory;
  std::shared_ptr<aasdk::usb::IAccessoryModeQueryChain> m_activeAoapQueryChain;
  std::shared_ptr<aasdk::usb::IUSBHub> m_usbHub;
#ifdef CRANKSHAFT_AASDK_OLD_API
  aasdk::usb::IAOAPDevice::Pointer m_aoapDevice;
#else
  std::shared_ptr<aasdk::usb::IAOAPDevice> m_aoapDevice;
#endif
  std::shared_ptr<aasdk::transport::ITransport> m_transport;
  std::shared_ptr<aasdk::tcp::TCPWrapper> m_tcpWrapper;
  std::shared_ptr<boost::asio::ip::tcp::acceptor> m_tcpAcceptor;
  std::shared_ptr<boost::asio::ip::tcp::socket> m_tcpSocket;
  std::shared_ptr<aasdk::messenger::ICryptor> m_cryptor;
  std::shared_ptr<aasdk::messenger::IMessenger> m_messenger;

  // AASDK channels
  std::shared_ptr<aasdk::channel::mediasink::video::channel::VideoChannel> m_videoChannel;
  std::shared_ptr<aasdk::channel::mediasink::audio::channel::MediaAudioChannel> m_mediaAudioChannel;
  std::shared_ptr<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>
      m_systemAudioChannel;
  std::shared_ptr<aasdk::channel::mediasink::audio::channel::GuidanceAudioChannel>
      m_speechAudioChannel;
  std::shared_ptr<aasdk::channel::mediasink::audio::channel::TelephonyAudioChannel>
      m_telephonyAudioChannel;
  std::shared_ptr<aasdk::channel::inputsource::InputSourceService> m_inputChannel;
  std::shared_ptr<aasdk::channel::sensorsource::SensorSourceService> m_sensorChannel;
  std::shared_ptr<aasdk::channel::bluetooth::BluetoothService> m_bluetoothChannel;
  std::shared_ptr<aasdk::channel::wifiprojection::IWifiProjectionService>
      m_wifiProjectionChannel;
  std::shared_ptr<aasdk::channel::mediasource::audio::MicrophoneAudioChannel> m_microphoneChannel;
  std::shared_ptr<aasdk::channel::control::ControlServiceChannel> m_controlChannel;
  std::shared_ptr<AAControlEventHandler> m_controlEventHandler;
  int m_controlVersionRequestAttempts{0};
  int m_controlVersionRequestMaxAttempts{10};
  qint64 m_controlVersionFirstRequestMs{0};
  qint64 m_controlVersionLastRequestMs{0};
  int m_controlSendNative2ConsecutiveTimeouts{0};
  int m_controlTimeoutRecoveryCount{0};
  bool m_forceCleanupUsbResetOnce{false};
  bool m_controlHandshakeAwaitingActivation{false};
  qint64 m_controlHandshakeStartedMs{0};
  quint64 m_controlHandshakeEpoch{0};
  int m_controlHandshakeActivationRetryCount{0};
  QList<QString> m_controlTraceWindow;
  quint64 m_controlTraceSequence{0};
  qint64 m_controlTraceStartMs{0};
  int m_controlTraceWindowSize{40};
  std::shared_ptr<AAVideoEventHandler> m_videoEventHandler;
  std::shared_ptr<AAAudioEventHandler> m_mediaAudioEventHandler;
  std::shared_ptr<AAAudioEventHandler> m_systemAudioEventHandler;
  std::shared_ptr<AAAudioEventHandler> m_speechAudioEventHandler;
  std::shared_ptr<AAAudioEventHandler> m_telephonyAudioEventHandler;
  std::shared_ptr<AAInputEventHandler> m_inputEventHandler;
  std::shared_ptr<AASensorEventHandler> m_sensorEventHandler;
  std::shared_ptr<AAMicrophoneEventHandler> m_microphoneEventHandler;
  std::shared_ptr<AABluetoothEventHandler> m_bluetoothEventHandler;
  std::shared_ptr<AAWifiProjectionEventHandler> m_wifiProjectionEventHandler;

  // Multimedia components
  std::unique_ptr<IVideoDecoder> m_videoDecoder;
  std::unique_ptr<IAudioMixer> m_audioMixer;

  bool m_isInitialised{false};
  libusb_context* m_libusbContext{nullptr};
};
