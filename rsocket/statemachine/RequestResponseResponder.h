// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "rsocket/statemachine/StreamStateMachineBase.h"
#include "yarpl/flowable/Subscriber.h"
#include "yarpl/single/SingleObserver.h"
#include "yarpl/single/SingleSubscription.h"

namespace rsocket {

/// Implementation of stream stateMachine that represents a RequestResponse
/// responder
class RequestResponseResponder : public StreamStateMachineBase,
                                 public yarpl::single::SingleObserver<Payload> {
 public:
  RequestResponseResponder(
      std::shared_ptr<StreamsWriter> writer,
      StreamId streamId)
      : StreamStateMachineBase(std::move(writer), streamId) {}

 private:
  void onSubscribe(yarpl::Reference<yarpl::single::SingleSubscription>
                       subscription) noexcept override;
  void onSuccess(Payload) noexcept override;
  void onError(folly::exception_wrapper) noexcept override;

  void handleCancel() override;

  void endStream(StreamCompletionSignal) override;

  /// State of the Subscription responder.
  enum class State : uint8_t {
    RESPONDING,
    CLOSED,
  } state_{State::RESPONDING};

  yarpl::Reference<yarpl::single::SingleSubscription> producingSubscription_;
};
}
