// Copyright (c) 2018 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/no_destructor.h"

#include <cstdint>
#include <cstdlib>
#include <utility>

#include "gtest/gtest.h"

namespace leveldb {

namespace {

struct DoNotDestruct {
 public:
  DoNotDestruct(uint32_t a, uint64_t b) : a(a), b(b) {}
  ~DoNotDestruct() { std::abort(); }

  // Used to check constructor argument forwarding.
  // 用于检查 构造器 的 参数转发
  uint32_t a;
  uint64_t b;
};

constexpr const uint32_t kGoldenA = 0xdeadbeef;
constexpr const uint64_t kGoldenB = 0xaabbccddeeffaabb;

}  // namespace

// 栈实例
TEST(NoDestructorTest, StackInstance) {
  NoDestructor<DoNotDestruct> instance(kGoldenA, kGoldenB);
  ASSERT_EQ(kGoldenA, instance.get()->a);
  ASSERT_EQ(kGoldenB, instance.get()->b);
}

// 静态实例
TEST(NoDestructorTest, StaticInstance) {
  static NoDestructor<DoNotDestruct> instance(kGoldenA, kGoldenB);
  ASSERT_EQ(kGoldenA, instance.get()->a);
  ASSERT_EQ(kGoldenB, instance.get()->b);
}

}  // namespace leveldb
