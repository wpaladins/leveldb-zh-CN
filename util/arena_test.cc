// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

#include "gtest/gtest.h"
#include "util/random.h"

namespace leveldb {

TEST(ArenaTest, Empty) { Arena arena; }

TEST(ArenaTest, Simple) {
  std::vector<std::pair<size_t, char*>> allocated;
  Arena arena;
  const int N = 100000;
  size_t bytes = 0;
  Random rnd(301);
  for (int i = 0; i < N; i++) {
    size_t s;
    if (i % (N / 10) == 0) {
      s = i;
    } else {
      // 1/4000 概率 s 在 6000 内
      // 剩余 3999/4000 概率
      //      1/10 概率 s 在 100 内
      //      9/10 概率 s 在 20 内
      s = rnd.OneIn(4000)
              ? rnd.Uniform(6000)
              : (rnd.OneIn(10) ? rnd.Uniform(100) : rnd.Uniform(20));
    }
    if (s == 0) {
      // Our arena disallows size 0 allocations.
      s = 1;
    }
    char* r;
    // 1/10 概率 对齐分配
    if (rnd.OneIn(10)) {
      r = arena.AllocateAligned(s);
    } else {
      r = arena.Allocate(s);
    }

    // 填充 可知值 以方便接下来验证
    for (size_t b = 0; b < s; b++) {
      // Fill the "i"th allocation with a known bit pattern
      r[b] = i % 256;
    }
    bytes += s;
    allocated.push_back(std::make_pair(s, r));
    // arene 的 总内存使用量 必然大于等于 bytes
    ASSERT_GE(arena.MemoryUsage(), bytes);
    // 当分配 足够多的 (1w 字节) 内存后, 多申请的内存 占 本应该申请的内存
    // 的比例小于 10%
    if (i > N / 10) {
      // arene 的 总内存使用量 必然小于等于 bytes * 1.10
      ASSERT_LE(arena.MemoryUsage(), bytes * 1.10);
    }
  }
  // 验证 值 的正确性
  for (size_t i = 0; i < allocated.size(); i++) {
    size_t num_bytes = allocated[i].first;
    const char* p = allocated[i].second;
    for (size_t b = 0; b < num_bytes; b++) {
      // Check the "i"th allocation for the known bit pattern
      ASSERT_EQ(int(p[b]) & 0xff, i % 256);
    }
  }
}

}  // namespace leveldb
