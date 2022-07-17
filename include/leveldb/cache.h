// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)
//
// Cache 是将 键 映射到 值 的接口.
// 它具有内部 同步, 可以安全地从多个线程同时访问.
// 它可能会 自动驱逐 条目以为新条目腾出空间.
// 值对缓存容量具有指定的 费用(charge).
// 例如, 值是 可变长度字符串 的缓存, 可以使用 字符串的长度 作为字符串的费用.
//
// 提供了具有 最近最少使用 (LRU) 的 驱逐策略的 内置缓存实现.
// 如果 clients 想要更复杂的东西 (如 扫描抵抗、自定义驱逐策略、可变缓存大小等),
// 他们可以使用他们自己的实现.

#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

namespace leveldb {

class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
// 创建具有 固定大小容量的 新 cache
// Cache 的这种实现使用 最近最少使用 (LRU) 的淘汰策略
LEVELDB_EXPORT Cache* NewLRUCache(size_t capacity);

class LEVELDB_EXPORT Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  // 通过调用传递给构造函数的 "deleter" 函数来销毁所有现有条目
  virtual ~Cache();

  // Opaque handle to an entry stored in the cache.
  // 存储在 cache 中的条目的 不透明 句柄
  struct Handle {};

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  //
  // Returns a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  //
  // When the inserted entry is no longer needed, the key and
  // value will be passed to "deleter".
  // 将 key->value 的 映射 插入到 cache 中, 并根据 总 cache 容量 为其分配指定的
  // charge
  //
  // 返回 对应于映射 的 句柄. 当不再需要 返回的映射 时, 调用者必须调用
  // this->Release(handle)
  //
  // 当不再需要插入的条目时, 键和值将传递给 "deleter"
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // If the cache has no mapping for "key", returns nullptr.
  //
  // Else return a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  // 如果 cache 没有 "key" 的映射, 则返回 nullptr
  //
  // 否则返回对应于映射的句柄. 当不再需要返回的映射时, 调用者必须调用
  // this->Release(handle)
  virtual Handle* Lookup(const Slice& key) = 0;

  // Release a mapping returned by a previous Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 释放先前 Lookup() 返回的映射
  // 要求：句柄必须尚未被释放
  // 要求：句柄必须是之前由 *this 上的方法返回的
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 返回封装在由成功的 Lookup() 返回的句柄中的值
  // 要求：句柄必须尚未被释放
  // 要求：句柄必须是之前由 *this 上的方法返回的
  virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it.  Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.
  // 如果 cache 包含 key 的条目, 则将其删除
  // 请注意, 底层条目 将一直保留, 直到它的 所有存在的句柄 都被释放
  virtual void Erase(const Slice& key) = 0;

  // Return a new numeric id.  May be used by multiple clients who are
  // sharing the same cache to partition the key space.  Typically the
  // client will allocate a new id at startup and prepend the id to
  // its cache keys.
  // 返回一个新的 数字 id. 可能被 共享相同 cache 的多个客户端 用于对键空间进行
  // 分区
  // 通常客户端会在启动时分配一个新的 id 并将 id 预先添加到它的 cache 键中
  virtual uint64_t NewId() = 0;

  // Remove all cache entries that are not actively in use.  Memory-constrained
  // applications may wish to call this method to reduce memory usage.
  // Default implementation of Prune() does nothing.  Subclasses are strongly
  // encouraged to override the default implementation.  A future release of
  // leveldb may change Prune() to a pure abstract method.
  // 删除所有未在使用中的缓存条目
  // 内存受限 的应用程序可能希望调用此方法来 减少 内存使用量
  // Prune() 的 默认实现 什么也不做
  // 强烈建议子类 override 默认实现
  // leveldb 的未来版本可能会将 Prune() 更改为 纯抽象方法
  virtual void Prune() {}

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  // 返回缓存中存储的 所有元素 的 合并 charges 的估计值
  virtual size_t TotalCharge() const = 0;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
