// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.
// LRU cache 实现
//
// Cache entries 有一个 "in_cache" 布尔值
// [用途] 指示 cache 是否对 entry 有引用
// [何时会变为 false] 在没有将 entry 传递给其 "deleter" 的情况下, 它可能变为
// false 的 所有途径 是
//     1. 通过 Erase(), [用于删除指定 key 对应的 entry]
//     2. 通过 Insert() 插入具有重复键的元素时
//     3. 在 cache 析构时
//
// cache 在 cache 中保留两个 items 的链接链表.
// cache 中的所有 items 都在一个链表 或 另一个链表中, 永远不会同时在两个链表中.
// 仍然被 clients 引用 但从 cache 中 删除 的 items 不在这两个链表中.
// 两个链表分别是:
//   - in-use: 包含 clients 当前引用的 items , 没有特定的顺序.
//             (此链表用于 不变量检查. 如果我们删除检查,
//             该链表中的元素可能会保留为 断开连接的单例链表.)
//   - LRU: 包含 clients 当前未引用的 items , 按 LRU 顺序排列
//
// 当 Ref() 和 Unref() 方法检测到 cache 中的 元素 acquiring 或 losing
// 它的 唯一的外部引用 时, 元素会在这两个链表之间移动.
//
// [note] in-use 和 LRU 两个链表中的 items 都会占用 cache 的容量

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
// An entry 是 可变长度的 堆分配结构
// Entries 保存在按 访问时间 排序的 循环双向链表 中
struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value);
  LRUHandle* next_hash;
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  bool in_cache;     // Whether entry is in the cache.
  uint32_t refs;     // References, including cache reference, if present.
  uint32_t hash;     // Hash of key(); used for fast sharding and comparisons
  char key_data[1];  // Beginning of key
  /*
   * value       : 值 的指针
   *   [note] 只存了 value 的指针, 因此 value 的 生命周期 不与 handle 强相关,
   *           可以选择性在 deleter 释放
   * deleter     : 删除器
   *   [note] 每个 handle 一个 删除器, 因此 cache 中的每个值的删除方式可以不同
   * charge      : 费用 (成本, 代价)
   *   [note] 代表了一个 handle 的大小, 用于控制 cache 的总大小
   * in_cache    : entry 是否在 缓存 中 (是否在 lru_ 或 in_use_ 中)
   * refs        : 引用, 包括 缓存引用 (如果存在)
   * hash        : key 的哈希; 用于快速分片和比较
   * key_data[1] : 键的开始
   *   [note] 1. 这种 key 的分配只能在 LRUHandle 在堆上动态分配时实现
   *          2. 在 LRUCache::Insert() 中 可以看到 分配一个 LRUHandle 的大小
   *          3. key_data 必须是 LRUHandle 的最后一个成员
   */

  Slice key() const {
    // next is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    // 仅当 LRU 句柄是 空链表 的 链表头 时, next 才等于 this.
    // 链表头永远不会有有意义的键
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
// 我们提供了我们自己的 简单哈希表, 因为它消除了一大堆 移植技巧,
// 并且比我们测试过的一些 编译器/运行时 组合中的一些 内置哈希表 实现更快.
// 例如, readrandom 比 g++ 4.4.3 的内置哈希表提高了 约5%
//
// key/hash => LRUHandle* 的哈希表
// hash 由 key 生成, 之所以 key 和 hash 同时比较, 应该是为了避免 哈希碰撞
// 导致的错误
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    // 以下赋值, 可能是覆盖了 old 在 链表 中的位置(前一项的 next_hash, 即原哈希表中已经存在 h->key(), h->hash 对应的节点),
    // 也可能是在 链表尾部 新增了项
    *ptr = h;
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        // 由于 每个缓存条目 都相当大, 我们的目标是 较小的 平均链表长度 (<= 1)
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  // 哈希表 由一组 存储桶 组成,
  // 其中每个 存储桶 是一个 cache entries 的链表,
  // 这些 cache entries 散列到存储桶中.
  //
  // 注意: 存储桶(链表) 并不是 list_, 而是 list_ 的元素
  // length_ 为 list_ 的长度, 即 存储桶 的个数
  // 存储桶 作为 cache entries 的链表, 是用 next_hash 链接起来的
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // 返回指向 与 key/hash 匹配的 缓存条目 的插槽 的指针.
  // 如果没有这样的 缓存条目, 则返回一个指向对应链表中 尾随槽 的指针.
  //
  // 通过这里的计算, 可以得知
  // hash 的 后 length 位 表明了 该 key 所在的存储桶
  //
  // 之所以返回 LRUHandle**, 是为了在返回 尾随槽 时, 可以方便插入
  // 也可以返回了一个 非 const 的 左值引用
  // [note] -> 的优先级高于 &
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  void Resize() {
    uint32_t new_length = 4;
    // 长度 以 2 的次方 增长
    while (new_length < elems_) {
      new_length *= 2;
    }
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    // 旧哈希桶(链表) => 新哈希桶(链表)
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        // ptr  为 指向 LRUHandle* 的指针, 它 指向 new_list (数组) 的一个位置
        // *ptr 为 new_list (数组) 中该位置的值 (LRUHandle* 类型)
        // h->next_hash = *ptr => 将 *h 插在 **ptr 的前面
        // *ptr = h            => 将 h 放入 new_list (数组) 中 *ptr 原本所在位置
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
// 分片缓存 的 单个分片
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  // 与构造函数分开, 这样调用者可以轻松地制作 LRUCache 的数组
  // (因为 数组使用 空参 构造函数)
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  // 像 Cache 的方法, 但有一个额外的 "hash" 参数 (hash 紧跟在 key 之后)
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  // 全作用域加锁
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Initialized before use.
  size_t capacity_;

  // mutex_ protects the following state.
  // mutex_ 保护 后面的状态 (所以 LRUCache 是 线程安全的)
  mutable port::Mutex mutex_;
  size_t usage_ GUARDED_BY(mutex_);

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  // LRU 链表 的 虚拟头结点
  // lru.prev 是最新 entry , lru.next 是最旧 entry  (双向链表)
  // 这些 entry 的 refs==1 并且 in_cache==true
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  // 在用链表 的 虚拟头结点
  // 这些 entry 正在被 clients 使用, 并且 refs>=1 并且 in_cache==true
  // in_use_ 中的 entry 如果有 refs==1 并且 in_cache==true
  // 说明该 entry 虽然仍然被 一个 client 持有, 但是已经在被 顶替/Erase 了
  LRUHandle in_use_ GUARDED_BY(mutex_);

  HandleTable table_ GUARDED_BY(mutex_);
};

LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  // 制作空的循环链表
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

LRUCache::~LRUCache() {
  // 如果 调用者 持有一个未释放的 handle, 则会出错
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  // 释放 lru_ 链表中的所有项
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    // lru_ 列表的不变量
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

// e->refs == 1 && e->in_cache 表明 e 对应的元素 在 lru_ 链表中
// 从 lru_ 链表中删除 => 这样就不会被 LRU 算法淘汰
// 添加到 in_use_ 链表
void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

// e->refs-- 后, 如果
//
// e->refs == 0 => 需要 释放 *e
//    情况1: 需要将 *e 从 lru_ 中淘汰时
//    情况2: *e 已经被 Erase(), 但 Erase() 之前仍然被 clients 持有 (此时 *e
//          已不在任何一个链表中), 当最后一个 client 释放 Release() 时
//
// e->in_cache && e->refs == 1 => 需要 将 e 追加到 lru_ 中
//    e->in_cache  表明 该元素并没有被 删除 或 顶替 [通过 Erase() 或 Insert()]
//    e->in_cache && e->refs == 1 共同表明 该元素 之前在 in_use_ 中
void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    // 不再使用; 移动到链表 lru_
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

// 从 e 原来所在的链 中删除 e
void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

// 将 e 追加到 *list 链中
void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  // 通过插入到 *list 之前 来使 "e" 成为 最新的 entry
  // 因为 *list 是双向链表, *list 之前即为 链表的尾部
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

// 全作用域加锁
// 查找指定 key & hash 的项是否存在于 cache 中
// 全作用域加锁 (保护对 两个链表 & 哈希表 的修改)
// 通过 哈希表 查找, 如果 找到了, 则 通过 Ref() 对引用执行修改
//     有可能 当前项 在 lru_ 中 => 移至 in_use_ 并对 refs++
//     也有可能 当前项 在 in_use_ 中 => 仅对 refs++
//     增加后, 必然有 refs >= 2
Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

// 全作用域加锁
// (clients) 调用时, 必然有 handle->refs >= 1
void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

// 全作用域加锁
Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 注意这里分配的 堆空间 大小
  LRUHandle* e =
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  // [note] 这里因为返回了一个 handle 所以 refs 初始值为 1
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  // 容量大于 0 的情况下, 将 e 插入 in_use_
  // 否则并不会插入 cache
  // 即, Insert() 之后, 默认 caller 持有一个该缓存的引用
  if (capacity_ > 0) {
    // [note] 这里因为需要将 *e 放入 cache, 所以 refs++, 即变为 2
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    LRU_Append(&in_use_, e);
    usage_ += charge;
    // 如果 key 对应的数据已经存在, 则 FinishErase() 会将旧数据 删除
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // next is read by key() in an assert, so it must be initialized
    // 不要缓存. (capacity_==0 是支持的, 用于 关闭缓存.)
    // next 由 LRUHandle::key() 中的 assert 读取, 因此必须对其进行初始化
    e->next = nullptr;
  }
  // 当 缓存使用量 > 缓存容量 并且 lru_ 非空 的情况下
  //     调用 FinishErase() 将 lru_ 头部的项 移除
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
// 如果 e != nullptr, 完成
//     从缓存中删除 *e;
// (在调用该方法之前) *e 已从 哈希表 中删除.
//     [因此 该方法的入参 e 应该为 哈希表的操作的返回值]
// 返回是否 e != nullptr
//
// FinishErase 会无条件的将 *e 从它所在的链表中移除, 但是并不会释放 它持有的空间
// 也就是 如果有 clients 仍然持有 *e, 仍然是可以使用的 (但是已经不占用 usage 了)
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }
  return e != nullptr;
}

// 全作用域加锁
void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  FinishErase(table_.Remove(key, hash));
}

// 全作用域加锁
// Prune() 删除数据库的 内存读取缓存, 以便客户端可以缓解其内存不足
void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}

static const int kNumShardBits = 4;
// 分片数量为 2 ^ 4 == 16
static const int kNumShards = 1 << kNumShardBits;

/**
 * 实现了 Cache 的接口
 * 将数据 分片 存储在 LRUCache 中
 */
class ShardedLRUCache : public Cache {
 private:
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  // 计算 每个分片的 容量大小 并设置
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  // 根据 key 的 哈希值 的 前 kNumShardBits 位 来决定使用哪个分片
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
