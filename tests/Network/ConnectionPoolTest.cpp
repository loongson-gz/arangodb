////////////////////////////////////////////////////////////////////////////////
/// @brief test suite for ConnectionPool
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "Network/ConnectionPool.h"

#include <fuerte/connection.h>
#include <fuerte/requests.h>

using namespace arangodb;
using namespace arangodb::network;

namespace {

void doNothing(fuerte::Error, std::unique_ptr<fuerte::Request> req,
              std::unique_ptr<fuerte::Response> res) {
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
};
}

TEST(NetworkConnectionPoolTest, acquire_endpoint) {
  ConnectionPool::Config config;
  config.numIOThreads = 1;
  config.minOpenConnections = 1;
  config.maxOpenConnections = 3;
  config.idleConnectionMilli = 10; // extra small for testing
  config.verifyHosts = false;
  config.protocol = fuerte::ProtocolType::Http;
  
  ConnectionPool pool(config);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  auto conn = pool.leaseConnection("tcp://example.org:80");
  ASSERT_EQ(pool.numOpenConnections(), 1);
  auto req = fuerte::createRequest(fuerte::RestVerb::Get, fuerte::ContentType::Unset);
  auto res = conn->sendRequest(std::move(req));
  ASSERT_EQ(res->statusCode(), fuerte::StatusOK);
  ASSERT_TRUE(res->payloadSize() > 0);
}

TEST(NetworkConnectionPoolTest, acquire_multiple_endpoint) {
  ConnectionPool::Config config;
  config.numIOThreads = 1;
  config.minOpenConnections = 1;
  config.maxOpenConnections = 3;
  config.idleConnectionMilli = 10; // extra small for testing
  config.verifyHosts = false;
  config.protocol = fuerte::ProtocolType::Http;
  
  ConnectionPool pool(config);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  auto conn1 = pool.leaseConnection("tcp://example.org:80");
  
  conn1->sendRequest(fuerte::createRequest(fuerte::RestVerb::Get,
                                           fuerte::ContentType::Unset), doNothing);
  
  auto conn2 = pool.leaseConnection("tcp://example.org:80");
  
  ASSERT_NE(conn1.get(), conn2.get());
  ASSERT_EQ(pool.numOpenConnections(), 2);
  
  auto conn3 = pool.leaseConnection("tcp://example.com:80");
  ASSERT_NE(conn1.get(), conn3.get());
  
  ASSERT_EQ(pool.numOpenConnections(), 3);
}

TEST(NetworkConnectionPoolTest, release_multiple_endpoints_one) {
  ConnectionPool::Config config;
  config.numIOThreads = 1;
  config.minOpenConnections = 1;
  config.maxOpenConnections = 3;
  config.idleConnectionMilli = 10; // extra small for testing
  config.verifyHosts = false;
  config.protocol = fuerte::ProtocolType::Http;
  
  ConnectionPool pool(config);

  
  {
    auto conn1 = pool.leaseConnection("tcp://example.org:80");
    ASSERT_EQ(pool.numOpenConnections(), 1);
    conn1->sendRequest(fuerte::createRequest(fuerte::RestVerb::Get,
                                             fuerte::ContentType::Unset), doNothing);
    
    auto conn2 = pool.leaseConnection("tcp://example.com:80");
    ASSERT_NE(conn1.get(), conn2.get());
    ASSERT_EQ(pool.numOpenConnections(), 2);
  }
  ASSERT_EQ(pool.numOpenConnections(), 2);
  
  std::this_thread::sleep_for(std::chrono::milliseconds(11));
  pool.pruneConnections();
  
  ASSERT_EQ(pool.numOpenConnections(), 2); // keep one endpoint each
}

TEST(NetworkConnectionPoolTest, release_multiple_endpoints_two) {
  ConnectionPool::Config config;
  config.numIOThreads = 1;
  config.minOpenConnections = 0;
  config.maxOpenConnections = 3;
  config.idleConnectionMilli = 10; // extra small for testing
  config.verifyHosts = false;
  config.protocol = fuerte::ProtocolType::Http;
  
  ConnectionPool pool(config);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  {
    auto conn1 = pool.leaseConnection("tcp://example.org:80");
    ASSERT_EQ(pool.numOpenConnections(), 1);
    conn1->sendRequest(fuerte::createRequest(fuerte::RestVerb::Get,
                                             fuerte::ContentType::Unset), doNothing);
    
    auto conn2 = pool.leaseConnection("tcp://example.com:80");
    ASSERT_NE(conn1.get(), conn2.get());
    ASSERT_EQ(pool.numOpenConnections(), 2);
  }
  ASSERT_EQ(pool.numOpenConnections(), 2);
  
  std::this_thread::sleep_for(std::chrono::milliseconds(21));
  pool.pruneConnections();
  
  ASSERT_EQ(pool.numOpenConnections(), 0);
}


TEST(NetworkConnectionPoolTest, checking_min_and_max_connections) {
  ConnectionPool::Config config;
  config.numIOThreads = 1;
  config.minOpenConnections = 1;
  config.maxOpenConnections = 2;
  config.idleConnectionMilli = 10; // extra small for testing
  config.verifyHosts = false;
  config.protocol = fuerte::ProtocolType::Http;
  
  ConnectionPool pool(config);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  
  {
    auto conn1 = pool.leaseConnection("tcp://example.org:80");
    ASSERT_EQ(pool.numOpenConnections(), 1);
    
    conn1->sendRequest(fuerte::createRequest(fuerte::RestVerb::Get,
                                             fuerte::ContentType::Unset), doNothing);
    
    auto conn2 = pool.leaseConnection("tcp://example.org:80");
    ASSERT_NE(conn1.get(), conn2.get());
    ASSERT_EQ(pool.numOpenConnections(), 2);
    
    conn2->sendRequest(fuerte::createRequest(fuerte::RestVerb::Get,
                                             fuerte::ContentType::Unset), doNothing);
    
    auto conn3 = pool.leaseConnection("tcp://example.org:80");
    ASSERT_NE(conn1.get(), conn3.get());
    ASSERT_EQ(pool.numOpenConnections(), 3);
  }
  ASSERT_EQ(pool.numOpenConnections(), 3);
  
  // 15ms > 10ms
  std::this_thread::sleep_for(std::chrono::milliseconds(21));
  pool.pruneConnections();
  
  ASSERT_EQ(pool.numOpenConnections(), 1);
}
