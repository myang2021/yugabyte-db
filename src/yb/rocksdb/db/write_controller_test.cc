//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#include <string>

#include <gtest/gtest.h>

#include "yb/rocksdb/db/write_controller.h"

#include "yb/rocksdb/env.h"

namespace rocksdb {

class WriteControllerTest : public testing::Test {};

class TimeSetEnv : public EnvWrapper {
 public:
  TimeSetEnv() : EnvWrapper(nullptr) {}
  uint64_t now_micros_ = 6666;
  uint64_t NowMicros() override { return now_micros_; }
};

TEST_F(WriteControllerTest, ChangeDelayRateTest) {
  TimeSetEnv env;
  WriteController controller(10000000u);
  auto delay_token_0 =
      controller.GetDelayToken(controller.delayed_write_rate());
  ASSERT_EQ(static_cast<uint64_t>(2000000),
            controller.GetDelay(&env, 20000000u));
  auto delay_token_1 = controller.GetDelayToken(2000000u);
  ASSERT_EQ(static_cast<uint64_t>(10000000),
            controller.GetDelay(&env, 20000000u));
  auto delay_token_2 = controller.GetDelayToken(1000000u);
  ASSERT_EQ(static_cast<uint64_t>(20000000),
            controller.GetDelay(&env, 20000000u));
  auto delay_token_3 = controller.GetDelayToken(20000000u);
  ASSERT_EQ(static_cast<uint64_t>(1000000),
            controller.GetDelay(&env, 20000000u));
  auto delay_token_4 =
      controller.GetDelayToken(controller.delayed_write_rate() * 2);
  ASSERT_EQ(static_cast<uint64_t>(500000),
            controller.GetDelay(&env, 20000000u));
}

TEST_F(WriteControllerTest, SanityTest) {
  WriteController controller(10000000u);
  auto stop_token_1 = controller.GetStopToken();
  auto stop_token_2 = controller.GetStopToken();

  ASSERT_TRUE(controller.IsStopped());
  stop_token_1.reset();
  ASSERT_TRUE(controller.IsStopped());
  stop_token_2.reset();
  ASSERT_FALSE(controller.IsStopped());

  TimeSetEnv env;

  auto delay_token_1 = controller.GetDelayToken(10000000u);
  ASSERT_EQ(static_cast<uint64_t>(2000000),
            controller.GetDelay(&env, 20000000u));

  env.now_micros_ += 1999900u;  // sleep debt 1000

  auto delay_token_2 = controller.GetDelayToken(10000000u);
  // Rate reset after changing the token.
  ASSERT_EQ(static_cast<uint64_t>(2000000),
            controller.GetDelay(&env, 20000000u));

  env.now_micros_ += 1999900u;  // sleep debt 1000

  // One refill: 10240 bytes allowed, 1000 used, 9240 left
  ASSERT_EQ(static_cast<uint64_t>(1124), controller.GetDelay(&env, 1000u));
  env.now_micros_ += 1124u;  // sleep debt 0

  delay_token_2.reset();
  // 1000 used, 8240 left
  ASSERT_EQ(static_cast<uint64_t>(0), controller.GetDelay(&env, 1000u));

  env.now_micros_ += 100u;  // sleep credit 100
  // 1000 used, 7240 left
  ASSERT_EQ(static_cast<uint64_t>(0), controller.GetDelay(&env, 1000u));

  env.now_micros_ += 100u;  // sleep credit 200
  // One refill: 10240 fileed, sleep credit generates 2000. 8000 used
  //             7240 + 10240 + 2000 - 8000 = 11480 left
  ASSERT_EQ(static_cast<uint64_t>(1024u), controller.GetDelay(&env, 8000u));

  env.now_micros_ += 200u;  // sleep debt 824
  // 1000 used, 10480 left.
  ASSERT_EQ(static_cast<uint64_t>(0), controller.GetDelay(&env, 1000u));

  env.now_micros_ += 200u;  // sleep debt 624
  // Out of bound sleep, still 10480 left
  ASSERT_EQ(static_cast<uint64_t>(3000624u),
            controller.GetDelay(&env, 30000000u));

  env.now_micros_ += 3000724u;  // sleep credit 100
  // 6000 used, 4480 left.
  ASSERT_EQ(static_cast<uint64_t>(0), controller.GetDelay(&env, 6000u));

  env.now_micros_ += 200u;  // sleep credit 300
  // One refill, credit 4480 balance + 3000 credit + 10240 refill
  // Use 8000, 9720 left
  ASSERT_EQ(static_cast<uint64_t>(1024u), controller.GetDelay(&env, 8000u));

  env.now_micros_ += 3024u;  // sleep credit 2000

  // 1720 left
  ASSERT_EQ(static_cast<uint64_t>(0u), controller.GetDelay(&env, 8000u));

  // 1720 balance + 20000 credit = 20170 left
  // Use 8000, 12170 left
  ASSERT_EQ(static_cast<uint64_t>(0u), controller.GetDelay(&env, 8000u));

  // 4170 left
  ASSERT_EQ(static_cast<uint64_t>(0u), controller.GetDelay(&env, 8000u));

  // Need a refill
  ASSERT_EQ(static_cast<uint64_t>(1024u), controller.GetDelay(&env, 9000u));

  delay_token_1.reset();
  ASSERT_EQ(static_cast<uint64_t>(0), controller.GetDelay(&env, 30000000u));
  delay_token_1.reset();
  ASSERT_FALSE(controller.IsStopped());
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
