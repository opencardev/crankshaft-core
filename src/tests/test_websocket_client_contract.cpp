#include <gtest/gtest.h>

#include "../services/websocket/WebSocketServer.h"

namespace {
using Config = WebSocketServer::ClientHelloContractConfig;
using Payload = WebSocketServer::ClientHelloPayload;
using Decision = WebSocketServer::ClientHelloDecision;

auto makePayload(const QString& kind,
                 const QString& version,
                 int protocol,
                 const QSet<QString>& capabilities) -> Payload {
  return Payload{kind, version, protocol, capabilities};
}
}

TEST(WebSocketClientContractTest, StrictModeRejectsProtocolMismatch) {
  const Config config{/*requireClientHello=*/true,
                      /*requiredClientProtocolVersion=*/2,
                      /*minClientVersionMajor=*/0,
                      /*requireAndroidAutoCapability=*/false};
  const Payload payload = makePayload(QStringLiteral("ui-slim"), QStringLiteral("1.2.0"), 1,
                                      {QStringLiteral("android_auto")});

  const Decision decision = WebSocketServer::evaluateClientHello(payload, config);
  EXPECT_EQ(decision, Decision::ProtocolMismatch);
  EXPECT_EQ(WebSocketServer::clientHelloDecisionError(decision),
            QStringLiteral("client_hello_protocol_mismatch"));
}

TEST(WebSocketClientContractTest, StrictModeRejectsMissingRequiredCapability) {
  const Config config{/*requireClientHello=*/true,
                      /*requiredClientProtocolVersion=*/1,
                      /*minClientVersionMajor=*/0,
                      /*requireAndroidAutoCapability=*/true};
  const Payload payload = makePayload(QStringLiteral("ui-slim"), QStringLiteral("1.2.0"), 1,
                                      {QStringLiteral("webrtc")});

  const Decision decision = WebSocketServer::evaluateClientHello(payload, config);
  EXPECT_EQ(decision, Decision::MissingRequiredCapability);
  EXPECT_EQ(WebSocketServer::clientHelloDecisionError(decision),
            QStringLiteral("client_hello_missing_required_capability"));
}

TEST(WebSocketClientContractTest, StrictModeAcceptsCompatibleClientHello) {
  const Config config{/*requireClientHello=*/true,
                      /*requiredClientProtocolVersion=*/1,
                      /*minClientVersionMajor=*/1,
                      /*requireAndroidAutoCapability=*/true};
  const Payload payload = makePayload(QStringLiteral("ui-slim"), QStringLiteral("2.3.4"), 1,
                                      {QStringLiteral("android_auto"), QStringLiteral("webrtc")});

  const Decision decision = WebSocketServer::evaluateClientHello(payload, config);
  EXPECT_EQ(decision, Decision::Accepted);
  EXPECT_TRUE(WebSocketServer::clientHelloDecisionError(decision).isEmpty());
}

TEST(WebSocketClientContractTest, ContractSatisfactionPermissiveModeBypassesHelloRequirement) {
  EXPECT_TRUE(WebSocketServer::isClientContractSatisfied(
      /*requireClientHello=*/false,
      /*clientHelloReceived=*/false));
}

TEST(WebSocketClientContractTest, ContractSatisfactionStrictModeRequiresHello) {
  EXPECT_FALSE(WebSocketServer::isClientContractSatisfied(
      /*requireClientHello=*/true,
      /*clientHelloReceived=*/false));
  EXPECT_TRUE(WebSocketServer::isClientContractSatisfied(
      /*requireClientHello=*/true,
      /*clientHelloReceived=*/true));
}
