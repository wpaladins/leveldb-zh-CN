// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <vector>

#include "util/coding.h"

#include "gtest/gtest.h"

namespace leveldb {

// Conversions between numeric keys/values and the types expected by Cache.
// 键/值的 数字类型 与 缓存期望的类型 之间的转换
static std::string EncodeKey(int k) {
  std::string result;
  PutFixed32(&result, k);
  return result;
}
static int DecodeKey(const Slice& k) {
  assert(k.size() == 4);
  return DecodeFixed32(k.data());
}
// uintptr_t 能够保存指向 void 的指针 的 无符号整数类型
// - 执行特定于整数的操作
// - 模糊指针的类型
// 目的是在 void* 类型的指针变量中 内联地 存储整型
// https://stackoverflow.com/a/1845491
static void* EncodeValue(uintptr_t v) { return reinterpret_cast<void*>(v); }
static int DecodeValue(void* v) { return reinterpret_cast<uintptr_t>(v); }

// 测试夹具
// 内部接口
// - 为了便于构造 测试数据
// - 对 Key 和 Value 的类型做了转换
// - 使用统一的 Deleter
class CacheTest : public testing::Test {
 public:
  static void Deleter(const Slice& key, void* v) {
    current_->deleted_keys_.push_back(DecodeKey(key));
    current_->deleted_values_.push_back(DecodeValue(v));
  }

  static constexpr int kCacheSize = 1000;
  std::vector<int> deleted_keys_;
  std::vector<int> deleted_values_;
  Cache* cache_;

  CacheTest() : cache_(NewLRUCache(kCacheSize)) { current_ = this; }

  ~CacheTest() { delete cache_; }

  // 查找 并 释放
  // 返回 value (未找到返回 -1)
  int Lookup(int key) {
    Cache::Handle* handle = cache_->Lookup(EncodeKey(key));
    const int r = (handle == nullptr) ? -1 : DecodeValue(cache_->Value(handle));
    if (handle != nullptr) {
      cache_->Release(handle);
    }
    return r;
  }

  // 插入 并 释放
  void Insert(int key, int value, int charge = 1) {
    cache_->Release(cache_->Insert(EncodeKey(key), EncodeValue(value), charge,
                                   &CacheTest::Deleter));
  }

  // 插入 但不 释放
  Cache::Handle* InsertAndReturnHandle(int key, int value, int charge = 1) {
    return cache_->Insert(EncodeKey(key), EncodeValue(value), charge,
                          &CacheTest::Deleter);
  }

  // 擦除 key
  void Erase(int key) { cache_->Erase(EncodeKey(key)); }
  // 为了配合 Deleter 单独保存每一个 测试用例中删除的 key/value
  static CacheTest* current_;
};
CacheTest* CacheTest::current_;

// TEST_F 可以直接访问 CacheTest 的 方法 及 变量
// 简单的插入后 命中 or 未命中 测试
TEST_F(CacheTest, HitAndMiss) {
  ASSERT_EQ(-1, Lookup(100));

  Insert(100, 101);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1, Lookup(200));
  ASSERT_EQ(-1, Lookup(300));

  Insert(200, 201);
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1, Lookup(300));

  // 重新设置 key(100) 对应的值, 会将 旧值 删除
  Insert(100, 102);
  ASSERT_EQ(102, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(-1, Lookup(300));

  ASSERT_EQ(1, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);
}

TEST_F(CacheTest, Erase) {
  // 空 cache 中
  Erase(200);
  ASSERT_EQ(0, deleted_keys_.size());

  // 存在的 key
  Insert(100, 101);
  Insert(200, 201);
  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);

  // 不存在的 key
  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(201, Lookup(200));
  ASSERT_EQ(1, deleted_keys_.size());
}

// 已固定 的 条目
// 指被 clients 持有 handle 的条目
// 他们会被存入 in_use_ 列表中, 被 顶替(Insert)/擦除(Erase) 之后并不会失效
// 当被 顶替/擦除 的条目的 所有被持有的 handle 都 Release 后, 条目才会被立即删除
TEST_F(CacheTest, EntriesArePinned) {
  Insert(100, 101);
  // [notice] 使用的是 cache_ 的 Lookup
  Cache::Handle* h1 = cache_->Lookup(EncodeKey(100));
  ASSERT_EQ(101, DecodeValue(cache_->Value(h1)));

  // 顶替 了旧值
  Insert(100, 102);
  Cache::Handle* h2 = cache_->Lookup(EncodeKey(100));
  ASSERT_EQ(102, DecodeValue(cache_->Value(h2)));
  // 由于 旧值 仍然被使用, 所以没有被删除
  ASSERT_EQ(0, deleted_keys_.size());

  cache_->Release(h1);
  ASSERT_EQ(1, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[0]);
  ASSERT_EQ(101, deleted_values_[0]);

  // 擦除 key, 只会将 key 从 cache 中移除, 在擦除之前拿到的 handle 仍然可以访问,
  // 只有在所有之前拿的 handle 都释放后才会将 key/value 删除
  Erase(100);
  ASSERT_EQ(-1, Lookup(100));
  ASSERT_EQ(1, deleted_keys_.size());

  cache_->Release(h2);
  ASSERT_EQ(2, deleted_keys_.size());
  ASSERT_EQ(100, deleted_keys_[1]);
  ASSERT_EQ(102, deleted_values_[1]);
}

// 驱逐策略
TEST_F(CacheTest, EvictionPolicy) {
  Insert(100, 101);
  Insert(200, 201);
  Insert(300, 301);
  // 将 key(300) 持有
  Cache::Handle* h = cache_->Lookup(EncodeKey(300));

  // Frequently used entry must be kept around,
  // as must things that are still in use.
  // 必须保留 经常使用的条目 (指 key(100)), 以及 仍在使用的东西 (指 key(300))
  // 连续插入 1100 次 (比 cache 大小(1000) 更大)
  for (int i = 0; i < kCacheSize + 100; i++) {
    Insert(1000 + i, 2000 + i);
    // 刚查完进行查询
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
    // 经常查询 100
    ASSERT_EQ(101, Lookup(100));
  }
  ASSERT_EQ(101, Lookup(100));
  ASSERT_EQ(-1, Lookup(200));
  ASSERT_EQ(301, Lookup(300));
  cache_->Release(h);
}

// 使用超过缓存大小
TEST_F(CacheTest, UseExceedsCacheSize) {
  // Overfill the cache, keeping handles on all inserted entries.
  // 过量 (1100) 填充 缓存, 保留所有 插入条目 的句柄
  std::vector<Cache::Handle*> h;
  for (int i = 0; i < kCacheSize + 100; i++) {
    h.push_back(InsertAndReturnHandle(1000 + i, 2000 + i));
  }

  // Check that all the entries can be found in the cache.
  // 检查是否可以在缓存中找到所有条目
  for (int i = 0; i < h.size(); i++) {
    ASSERT_EQ(2000 + i, Lookup(1000 + i));
  }

  for (int i = 0; i < h.size(); i++) {
    cache_->Release(h[i]);
  }
}

// 重条目
TEST_F(CacheTest, HeavyEntries) {
  // Add a bunch of light and heavy entries and then count the combined
  // size of items still in the cache, which must be approximately the
  // same as the total capacity.
  // 添加一堆 轻量级 和 重量级 条目, 然后计算仍在缓存中的条目的 组合大小,
  // 该大小必须与总容量大致相同
  const int kLight = 1;
  const int kHeavy = 10;
  int added = 0;
  int index = 0;
  // 一重一轻 交替存入 cache (过量填充 cache)
  while (added < 2 * kCacheSize) {
    const int weight = (index & 1) ? kLight : kHeavy;
    Insert(index, 1000 + index, weight);
    added += weight;
    index++;
  }

  // 计算缓存下的总重量, 并验证缓存的值
  int cached_weight = 0;
  for (int i = 0; i < index; i++) {
    const int weight = (i & 1 ? kLight : kHeavy);
    int r = Lookup(i);
    if (r >= 0) {
      cached_weight += weight;
      ASSERT_EQ(1000 + i, r);
    }
  }
  // 缓存的总重量 <= 缓存大小的 1.1 倍
  ASSERT_LE(cached_weight, kCacheSize + kCacheSize / 10);
}

TEST_F(CacheTest, NewId) {
  uint64_t a = cache_->NewId();
  uint64_t b = cache_->NewId();
  ASSERT_NE(a, b);
}

// Prune 只会将存在 lru_ 列表中的条目 移除(Erase)
// 被 clients 只有 handle 的条目, 在 in_use_ 列表中, 不会被移除
// (释放后也不会被移除)
TEST_F(CacheTest, Prune) {
  Insert(1, 100);
  Insert(2, 200);

  Cache::Handle* handle = cache_->Lookup(EncodeKey(1));
  ASSERT_TRUE(handle);
  cache_->Prune();
  cache_->Release(handle);

  ASSERT_EQ(100, Lookup(1));
  ASSERT_EQ(-1, Lookup(2));
}

// 0 大小的 cache, 可以插入, 但不会缓存
// 插入后的 handle 可以使用
// 但 handle 释放后, 缓存会被立即清理
TEST_F(CacheTest, ZeroSizeCache) {
  delete cache_;
  cache_ = NewLRUCache(0);

  Insert(1, 100);
  ASSERT_EQ(-1, Lookup(1));
}

}  // namespace leveldb
