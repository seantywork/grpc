//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/grpc.h>

#include <array>
#include <memory>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "gtest/gtest.h"
#include "src/core/lib/experiments/experiments.h"
#include "src/core/resolver/endpoint_addresses.h"
#include "src/core/util/orphanable.h"
#include "src/core/util/ref_counted_ptr.h"
#include "test/core/load_balancing/lb_policy_test_lib.h"
#include "test/core/test_util/test_config.h"

namespace grpc_core {
namespace testing {
namespace {

class RoundRobinTest : public LoadBalancingPolicyTest {
 protected:
  RoundRobinTest() : LoadBalancingPolicyTest("round_robin") {}
};

TEST_F(RoundRobinTest, Basic) {
  const std::array<absl::string_view, 3> kAddresses = {
      "ipv4:127.0.0.1:441", "ipv4:127.0.0.1:442", "ipv4:127.0.0.1:443"};
  EXPECT_EQ(ApplyUpdate(BuildUpdate(kAddresses, nullptr), lb_policy()),
            absl::OkStatus());
  ExpectRoundRobinStartup(kAddresses);
}

TEST_F(RoundRobinTest, AddressUpdates) {
  const std::array<absl::string_view, 3> kAddresses = {
      "ipv4:127.0.0.1:441", "ipv4:127.0.0.1:442", "ipv4:127.0.0.1:443"};
  EXPECT_EQ(ApplyUpdate(BuildUpdate(kAddresses, nullptr), lb_policy()),
            absl::OkStatus());
  ExpectRoundRobinStartup(kAddresses);
  // Send update to remove address 2.
  EXPECT_EQ(
      ApplyUpdate(BuildUpdate(absl::MakeSpan(kAddresses).first(2), nullptr),
                  lb_policy()),
      absl::OkStatus());
  WaitForRoundRobinListChange(kAddresses, absl::MakeSpan(kAddresses).first(2));
  // Send update to remove address 0 and re-add address 2.
  EXPECT_EQ(
      ApplyUpdate(BuildUpdate(absl::MakeSpan(kAddresses).last(2), nullptr),
                  lb_policy()),
      absl::OkStatus());
  WaitForRoundRobinListChange(absl::MakeSpan(kAddresses).first(2),
                              absl::MakeSpan(kAddresses).last(2));
}

TEST_F(RoundRobinTest, MultipleAddressesPerEndpoint) {
  constexpr std::array<absl::string_view, 2> kEndpoint1Addresses = {
      "ipv4:127.0.0.1:443", "ipv4:127.0.0.1:444"};
  constexpr std::array<absl::string_view, 2> kEndpoint2Addresses = {
      "ipv4:127.0.0.1:445", "ipv4:127.0.0.1:446"};
  const std::array<EndpointAddresses, 2> kEndpoints = {
      MakeEndpointAddresses(kEndpoint1Addresses),
      MakeEndpointAddresses(kEndpoint2Addresses)};
  EXPECT_EQ(ApplyUpdate(BuildUpdate(kEndpoints, nullptr), lb_policy_.get()),
            absl::OkStatus());
  // RR should have created a subchannel for each address.
  auto* subchannel1_0 = FindSubchannel(kEndpoint1Addresses[0]);
  ASSERT_NE(subchannel1_0, nullptr) << "Address: " << kEndpoint1Addresses[0];
  auto* subchannel1_1 = FindSubchannel(kEndpoint1Addresses[1]);
  ASSERT_NE(subchannel1_1, nullptr) << "Address: " << kEndpoint1Addresses[1];
  auto* subchannel2_0 = FindSubchannel(kEndpoint2Addresses[0]);
  ASSERT_NE(subchannel2_0, nullptr) << "Address: " << kEndpoint2Addresses[0];
  auto* subchannel2_1 = FindSubchannel(kEndpoint2Addresses[1]);
  ASSERT_NE(subchannel2_1, nullptr) << "Address: " << kEndpoint2Addresses[1];
  // PF for each endpoint should try to connect to the first subchannel.
  EXPECT_TRUE(subchannel1_0->ConnectionRequested());
  EXPECT_FALSE(subchannel1_1->ConnectionRequested());
  EXPECT_TRUE(subchannel2_0->ConnectionRequested());
  EXPECT_FALSE(subchannel2_1->ConnectionRequested());
  // In the first endpoint, the first subchannel reports CONNECTING.
  // This causes RR to report CONNECTING.
  subchannel1_0->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  ExpectConnectingUpdate();
  // In the second endpoint, the first subchannel reports CONNECTING.
  subchannel2_0->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // In the first endpoint, the first subchannel fails to connect.
  // This causes PF to start a connection attempt on the second subchannel.
  subchannel1_0->SetConnectivityState(GRPC_CHANNEL_TRANSIENT_FAILURE,
                                      absl::UnavailableError("ugh"));
  EXPECT_TRUE(subchannel1_1->ConnectionRequested());
  subchannel1_1->SetConnectivityState(GRPC_CHANNEL_CONNECTING);
  // In the second endpoint, the first subchannel becomes connected.
  // This causes RR to report READY with all RPCs going to a single address.
  subchannel2_0->SetConnectivityState(GRPC_CHANNEL_READY);
  auto picker = WaitForConnected();
  ExpectRoundRobinPicks(picker.get(), {kEndpoint2Addresses[0]});
  // In the first endpoint, the second subchannel becomes connected.
  // This causes RR to add it to the rotation.
  subchannel1_1->SetConnectivityState(GRPC_CHANNEL_READY);
  WaitForRoundRobinListChange({kEndpoint2Addresses[0]},
                              {kEndpoint1Addresses[1], kEndpoint2Addresses[0]});
  // No more connection attempts triggered.
  EXPECT_FALSE(subchannel1_0->ConnectionRequested());
  EXPECT_FALSE(subchannel1_1->ConnectionRequested());
  EXPECT_FALSE(subchannel2_0->ConnectionRequested());
  EXPECT_FALSE(subchannel2_1->ConnectionRequested());
  // First endpoint first subchannel finishes backoff, but this doesn't
  // affect anything -- in fact, PF isn't even watching this subchannel
  // anymore, since it's connected to the other one.  However, this
  // ensures that the subchannel is in the right state when we try to
  // reconnect below.
  subchannel1_0->SetConnectivityState(GRPC_CHANNEL_IDLE);
  EXPECT_FALSE(subchannel1_0->ConnectionRequested());
  // Endpoint 1 switches to a different address.
  ExpectEndpointAddressChange(kEndpoint1Addresses, 1, 0, [&]() {
    // RR will remove the endpoint from the rotation when it becomes
    // disconnected.
    WaitForRoundRobinListChange(
        {kEndpoint1Addresses[1], kEndpoint2Addresses[0]},
        {kEndpoint2Addresses[0]});
  });
  // Then RR will re-add the endpoint with the new address.
  WaitForRoundRobinListChange({kEndpoint2Addresses[0]},
                              {kEndpoint1Addresses[0], kEndpoint2Addresses[0]});
  // No more connection attempts triggered.
  EXPECT_FALSE(subchannel1_0->ConnectionRequested());
  EXPECT_FALSE(subchannel1_1->ConnectionRequested());
  EXPECT_FALSE(subchannel2_0->ConnectionRequested());
  EXPECT_FALSE(subchannel2_1->ConnectionRequested());
}

TEST_F(RoundRobinTest, StartsConnectingFromRandomIndex) {
  if (!IsRrWrrConnectFromRandomIndexEnabled()) {
    GTEST_SKIP() << "rr_wrr_connect_from_random_index experiment not enabled";
  }
  const std::array<absl::string_view, 3> kAddresses = {
      "ipv4:127.0.0.1:441", "ipv4:127.0.0.1:442", "ipv4:127.0.0.1:443"};
  std::vector<absl::string_view> connect_order;
  request_connection_callback_ = [&](absl::string_view address) {
    connect_order.push_back(address);
  };
  // On each address list update, we choose a random index to start
  // connecting from rather than using index 0.  However, the random
  // index might happen to be 0 on any given attempt.  We try 10 times
  // to get one that is non-zero.
  // Note that we send the same address list on every update.  But since
  // we never told the subchannels to actually change their state when
  // they were asked to connect, they will continue to report IDLE, so
  // each successive update will re-request connection attempts.
  for (size_t i = 0; i < 10; ++i) {
    connect_order.clear();
    EXPECT_EQ(ApplyUpdate(BuildUpdate(kAddresses, nullptr), lb_policy()),
              absl::OkStatus());
    ASSERT_FALSE(connect_order.empty());
    if (connect_order[0] != kAddresses[0]) {
      EXPECT_THAT(
          connect_order,
          ::testing::AnyOf(::testing::ElementsAre(kAddresses[1], kAddresses[2],
                                                  kAddresses[0]),
                           ::testing::ElementsAre(kAddresses[2], kAddresses[0],
                                                  kAddresses[1])));
      // We started on an index other than 0, so we're done.
      return;
    }
    EXPECT_THAT(connect_order, ::testing::ElementsAreArray(kAddresses));
  }
  FAIL() << "all attempts started connecting at index 0";
}

// TODO(roth): Add test cases:
// - empty address list
// - subchannels failing connection attempts

}  // namespace
}  // namespace testing
}  // namespace grpc_core

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  return RUN_ALL_TESTS();
}
