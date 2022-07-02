// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/comparator.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>

#include "leveldb/slice.h"
#include "util/logging.h"
#include "util/no_destructor.h"

namespace leveldb {

Comparator::~Comparator() = default;

// 匿名类 实现 文件作用域
namespace {
class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() = default;

  const char* Name() const override { return "leveldb.BytewiseComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }

  // 缩短 *start
  // 但要保持 *start < limit 的关系
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    // Find length of common prefix
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other
    } else {
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        // *start 中 与 limit 不同的 第一个字符(非 0xff 时) + 1
        (*start)[diff_index]++;
        // 注意! 这里将 *start 下标 diff_index 之后的字符丢掉了
        start->resize(diff_index + 1);
        assert(Compare(*start, limit) < 0);
      }
    }
  }

  // 寻找一个 key 的 短的 继任者
  // 找到第一个不是 0xff 的字符, 对其加 1, 并将它所在位置之后的字符删掉
  void FindShortSuccessor(std::string* key) const override {
    // Find first character that can be incremented
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i + 1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
    // *key 由 0xffs 组成. 不要管它.
  }
};
}  // namespace

// 申明 在 comparator.h 中
// 为什么要没有析构函数?
// 参考:
//     https://111qqz.com/2022/03/leveldb-notes-02/#why-use-nodestructor
//     https://www.zhihu.com/question/497429375/answer/2304572264
// 总结: 单例模式
//       使用 函数级别 的静态变量 - 懒加载
//        - 相较于 chromium 中的 base::LazyInstance, 该方法在实现懒加载的同时,
//          支持对构造函数中传入参数
//       使用 NoDestructor 来避免 析构 的调用
//        - 避免因为 程序退出时析构被调用 而产生的 奇怪的 难于追踪的 问题
//        - 也可以用 静态指针 + new 解决这一问题, 但会因在 堆上分配内存
//          而降低效率, 而 NoDestructor 使用的 内联存储 (inline storage) 在
//          静态存储区 存储变量
const Comparator* BytewiseComparator() {
  static NoDestructor<BytewiseComparatorImpl> singleton;
  return singleton.get();
}

}  // namespace leveldb
