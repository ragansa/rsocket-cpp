// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/statemachine/RequestResponseRequester.h"

#include "rsocket/internal/Common.h"
#include "rsocket/statemachine/RSocketStateMachine.h"

namespace rsocket {

using namespace yarpl;
using namespace yarpl::flowable;

void RequestResponseRequester::subscribe(
    yarpl::Reference<yarpl::single::SingleObserver<Payload>> subscriber) {
  DCHECK(!isTerminated());
  DCHECK(!consumingSubscriber_);
  consumingSubscriber_ = std::move(subscriber);
  consumingSubscriber_->onSubscribe(this->ref_from_this(this));

  if (state_ == State::NEW) {
    state_ = State::REQUESTED;
    newStream(StreamType::REQUEST_RESPONSE, 1, std::move(initialPayload_));
  } else {
    if (auto subscriber = std::move(consumingSubscriber_)) {
      subscriber->onError(
          std::runtime_error("cannot request more than 1 item"));
    }
    closeStream(StreamCompletionSignal::ERROR);
  }
}

void RequestResponseRequester::cancel() noexcept {
  consumingSubscriber_ = nullptr;
  switch (state_) {
    case State::NEW:
      state_ = State::CLOSED;
      closeStream(StreamCompletionSignal::CANCEL);
      break;
    case State::REQUESTED: {
      state_ = State::CLOSED;
      cancelStream();
      closeStream(StreamCompletionSignal::CANCEL);
    } break;
    case State::CLOSED:
      break;
  }
  consumingSubscriber_ = nullptr;
}

void RequestResponseRequester::endStream(StreamCompletionSignal signal) {
  switch (state_) {
    case State::NEW:
    case State::REQUESTED:
      // Spontaneous ::endStream signal means an error.
      DCHECK(StreamCompletionSignal::COMPLETE != signal);
      DCHECK(StreamCompletionSignal::CANCEL != signal);
      state_ = State::CLOSED;
      break;
    case State::CLOSED:
      break;
  }
  if (auto subscriber = std::move(consumingSubscriber_)) {
    DCHECK(signal != StreamCompletionSignal::COMPLETE);
    DCHECK(signal != StreamCompletionSignal::CANCEL);
    subscriber->onError(StreamInterruptedException(static_cast<int>(signal)));
  }
}

void RequestResponseRequester::handleError(
    folly::exception_wrapper errorPayload) {
  switch (state_) {
    case State::NEW:
      // Cannot receive a frame before sending the initial request.
      CHECK(false);
      break;
    case State::REQUESTED:
      state_ = State::CLOSED;
      if (auto subscriber = std::move(consumingSubscriber_)) {
        subscriber->onError(errorPayload);
      }
      closeStream(StreamCompletionSignal::ERROR);
      break;
    case State::CLOSED:
      break;
  }
}

void RequestResponseRequester::handlePayload(
    Payload&& payload,
    bool complete,
    bool flagsNext) {
  switch (state_) {
    case State::NEW:
      // Cannot receive a frame before sending the initial request.
      CHECK(false);
      break;
    case State::REQUESTED:
      state_ = State::CLOSED;
      break;
    case State::CLOSED:
      // should not be receiving frames when closed
      // if we ended up here, we broke some internal invariant of the class
      CHECK(false);
      break;
  }

  if (payload || flagsNext) {
    consumingSubscriber_->onSuccess(std::move(payload));
    consumingSubscriber_ = nullptr;
  } else if (!complete) {
    errorStream("payload, NEXT or COMPLETE flag expected");
    return;
  }
  closeStream(StreamCompletionSignal::COMPLETE);
}

size_t RequestResponseRequester::getConsumerAllowance() const {
  return (state_ == State::REQUESTED) ? 1 : 0;
}
}
