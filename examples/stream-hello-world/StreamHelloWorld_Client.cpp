// Copyright 2004-present Facebook. All Rights Reserved.

#include <iostream>

#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>

#include "examples/util/ExampleSubscriber.h"
#include "rsocket/RSocket.h"
#include "rsocket/transports/tcp/TcpConnectionFactory.h"

#include "yarpl/Flowable.h"

using namespace rsocket_example;
using namespace rsocket;

DEFINE_string(host, "localhost", "host to connect to");
DEFINE_int32(port, 9898, "host:port to connect to");

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = 0;
  folly::init(&argc, &argv);

  folly::ScopedEventBaseThread worker;

  folly::SocketAddress address;
  address.setFromHostPort(FLAGS_host, FLAGS_port);

  auto client = RSocket::createConnectedClient(
      std::make_unique<TcpConnectionFactory>(*worker.getEventBase(),
                                             std::move(address)))
      .get();

  client->getRequester()
      ->requestStream(Payload("Jane"))
      ->subscribe([](Payload p) {
        std::cout << "Received: " << p.moveDataToString() << std::endl;
      });

  // Wait for a newline on the console to terminate the server.
  std::getchar();

  return 0;
}
