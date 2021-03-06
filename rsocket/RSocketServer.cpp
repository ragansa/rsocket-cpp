// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/RSocketServer.h"
#include <folly/io/async/EventBaseManager.h>

#include <rsocket/internal/ScheduledRSocketResponder.h>
#include "rsocket/RSocketErrors.h"
#include "rsocket/RSocketStats.h"
#include "rsocket/framing/FramedDuplexConnection.h"
#include "rsocket/framing/ScheduledFrameTransport.h"
#include "rsocket/internal/ConnectionSet.h"

namespace rsocket {

RSocketServer::RSocketServer(
    std::unique_ptr<ConnectionAcceptor> connectionAcceptor,
    std::shared_ptr<RSocketStats> stats)
    : duplexConnectionAcceptor_(std::move(connectionAcceptor)),
      setupResumeAcceptors_([] {
        return new rsocket::SetupResumeAcceptor{
            ProtocolVersion::Unknown,
            folly::EventBaseManager::get()->getExistingEventBase()
          };
      }),
      connectionSet_(std::make_shared<ConnectionSet>()),
      stats_(std::move(stats)) {}

RSocketServer::~RSocketServer() {
  VLOG(3) << "~RSocketServer ..";
  shutdownAndWait();
}

void RSocketServer::shutdownAndWait() {
  if (isShutdown_) {
    return;
  }

  // Will stop forwarding connections from duplexConnectionAcceptor_ to
  // setupResumeAcceptors_
  isShutdown_ = true;

  // Stop accepting new connections.
  if (duplexConnectionAcceptor_) {
    duplexConnectionAcceptor_->stop();
  }

  std::vector<folly::Future<folly::Unit>> closingFutures;
  for (auto& acceptor : setupResumeAcceptors_.accessAllThreads()) {
    // This call will queue up the cleanup on the eventBase.
    closingFutures.push_back(acceptor.close());
  }

  folly::collectAll(closingFutures).get();

  // Close off all outstanding connections.
  connectionSet_.reset();
}

void RSocketServer::start(
    std::shared_ptr<RSocketServiceHandler> serviceHandler) {
  CHECK(duplexConnectionAcceptor_); // RSocketServer has to be initialized with
  // the acceptor

  if (started) {
    throw std::runtime_error("RSocketServer::start() already called.");
  }
  started = true;

  duplexConnectionAcceptor_->start([this, serviceHandler](
      std::unique_ptr<DuplexConnection> connection,
      folly::EventBase& eventBase) {
    acceptConnection(std::move(connection), eventBase, serviceHandler);
  });
}

void RSocketServer::start(OnNewSetupFn onNewSetupFn) {
  start(RSocketServiceHandler::create(std::move(onNewSetupFn)));
}

void RSocketServer::startAndPark(OnNewSetupFn onNewSetupFn) {
  startAndPark(RSocketServiceHandler::create(std::move(onNewSetupFn)));
}

void RSocketServer::acceptConnection(
    std::unique_ptr<DuplexConnection> connection,
    folly::EventBase&,
    std::shared_ptr<RSocketServiceHandler> serviceHandler) {
  stats_->serverConnectionAccepted();
  if (isShutdown_) {
    // connection is getting out of scope and terminated
    return;
  }

  std::unique_ptr<DuplexConnection> framedConnection;
  if (connection->isFramed()) {
    framedConnection = std::move(connection);
  } else {
    framedConnection = std::make_unique<FramedDuplexConnection>(
        std::move(connection), ProtocolVersion::Unknown);
  }

  auto* acceptor = setupResumeAcceptors_.get();

  VLOG(2) << "Going to accept duplex connection";

  acceptor->accept(
      std::move(framedConnection),
      std::bind(
          &RSocketServer::onRSocketSetup,
          this,
          serviceHandler,
          std::placeholders::_1,
          std::placeholders::_2),
      std::bind(
          &RSocketServer::onRSocketResume,
          this,
          serviceHandler,
          std::placeholders::_1,
          std::placeholders::_2));
}

void RSocketServer::onRSocketSetup(
    std::shared_ptr<RSocketServiceHandler> serviceHandler,
    yarpl::Reference<FrameTransport> frameTransport,
    SetupParameters setupParams) {
  auto eventBase = folly::EventBaseManager::get()->getExistingEventBase();
  VLOG(2) << "Received new setup payload on " << eventBase->getName();
  CHECK(eventBase);
  auto result = serviceHandler->onNewSetup(setupParams);
  if (result.hasError()) {
    VLOG(3) << "Terminating SETUP attempt from client.  No Responder";
    throw result.error();
  }
  auto connectionParams = result.value();
  if (!connectionParams.responder) {
    LOG(ERROR) << "Received invalid Responder. Dropping connection";
    throw RSocketException("Received invalid Responder from server");
  }
  auto responder = std::make_shared<ScheduledRSocketResponder>(
      std::move(connectionParams.responder), *eventBase);
  auto rs = std::make_shared<RSocketStateMachine>(
      std::move(responder),
      nullptr,
      RSocketMode::SERVER,
      std::move(connectionParams.stats),
      std::move(connectionParams.connectionEvents),
      nullptr, /* resumeManager */
      nullptr /* coldResumeHandler */);

  connectionSet_->insert(rs, eventBase);
  rs->registerSet(connectionSet_);

  auto requester = std::make_shared<RSocketRequester>(rs, *eventBase);
  auto serverState = std::shared_ptr<RSocketServerState>(
      new RSocketServerState(*eventBase, rs, requester));
  serviceHandler->onNewRSocketState(std::move(serverState), setupParams.token);
  rs->connectServer(std::move(frameTransport), std::move(setupParams));
}

void RSocketServer::onRSocketResume(
    std::shared_ptr<RSocketServiceHandler> serviceHandler,
    yarpl::Reference<FrameTransport> frameTransport,
    ResumeParameters resumeParams) {
  auto result = serviceHandler->onResume(resumeParams.token);
  if (result.hasError()) {
    stats_->resumeFailedNoState();
    VLOG(3) << "Terminating RESUME attempt from client.  No ServerState found";
    throw result.error();
  }
  auto serverState = std::move(result.value());
  CHECK(serverState);
  auto* eventBase = folly::EventBaseManager::get()->getExistingEventBase();
  VLOG(2) << "Resuming client on " << eventBase->getName();
  if (!serverState->eventBase_.isInEventBaseThread()) {
    // If the resumed connection is on a different EventBase, then use
    // ScheduledFrameTransport and ScheduledFrameProcessor to ensure the
    // RSocketStateMachine continues to live on the same EventBase and the
    // IO happens in the new EventBase
    auto scheduledFT = yarpl::make_ref<ScheduledFrameTransport>(
        std::move(frameTransport),
        eventBase, /* Transport EventBase */
        &serverState->eventBase_); /* StateMachine EventBase */
    serverState->eventBase_.runInEventBaseThread([
      serverState,
      scheduledFT = std::move(scheduledFT),
      resumeParams = std::move(resumeParams)
    ]() {
      serverState->rSocketStateMachine_->resumeServer(
          std::move(scheduledFT), resumeParams);
    });
  } else {
    // If the resumed connection is on the same EventBase, then the
    // RSocketStateMachine and Transport can continue living in the same
    // EventBase without any thread hopping between them.
    serverState->rSocketStateMachine_->resumeServer(
        std::move(frameTransport), resumeParams);
  }
}

void RSocketServer::startAndPark(
    std::shared_ptr<RSocketServiceHandler> serviceHandler) {
  start(std::move(serviceHandler));
  waiting_.wait();
}

void RSocketServer::unpark() {
  waiting_.post();
}

folly::Optional<uint16_t> RSocketServer::listeningPort() const {
  return duplexConnectionAcceptor_ ? duplexConnectionAcceptor_->listeningPort()
                                   : folly::none;
}

} // namespace rsocket
