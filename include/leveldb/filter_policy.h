// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in leveldb and are consulted
// automatically by leveldb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).
//
// 可以使用自定义 FilterPolicy 对象 配置数据库.
// 该对象负责从 一组键 中创建一个 小型过滤器.
// 这些过滤器存储在 leveldb 中, 由 leveldb 自动查询以决定是否从磁盘读取一些信息.
// 在许多情况下, 过滤器可以将每次 DB::Get() 调用的 磁盘查找次数 从 一把 减少到
// 一次 磁盘查找.
//
// 大多数人都希望使用内置的 布隆过滤器 (参见下面的 NewBloomFilterPolicy())

#ifndef STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
#define STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

class LEVELDB_EXPORT FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // Return the name of this policy.  Note that if the filter encoding
  // changes in an incompatible way, the name returned by this method
  // must be changed.  Otherwise, old incompatible filters may be
  // passed to methods of this type.
  //
  // 返回此策略的名称.
  // 请注意, 如果 过滤器编码 以 不兼容 的方式更改, 则必须更改此方法返回的名称.
  // 否则, 旧的 不兼容过滤器 可能会传递给这种类型的方法.
  virtual const char* Name() const = 0;

  // keys[0,n-1] contains a list of keys (potentially with duplicates)
  // that are ordered according to the user supplied comparator.
  // Append a filter that summarizes keys[0,n-1] to *dst.
  //
  // Warning: do not change the initial contents of *dst.  Instead,
  // append the newly constructed filter to *dst.
  //
  // keys[0,n-1] 包含根据 用户提供的 比较器 排序的 key 列表 (可能具有重复项).
  // 追加一个过滤器, 将 keys[0,n-1] 汇总到 *dst
  //
  // 警告: 不要更改 *dst 的初始内容. 而是, 将 新构建 的过滤器 追加 到 *dst.
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  // "filter" contains the data appended by a preceding call to
  // CreateFilter() on this class.  This method must return true if
  // the key was in the list of keys passed to CreateFilter().
  // This method may return true or false if the key was not on the
  // list, but it should aim to return false with a high probability.
  //
  // "filter" 包含由前面调用该类的 CreateFilter() 附加的数据.
  // 如果 key 在传递给 CreateFilter() 的 key 列表 中, 则此方法 必须 返回 true.
  // 如果 key 不在列表中, 此方法可能会返回 true 或 false,
  // 但它应该旨在以高概率返回 false
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Return a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key.  A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
//
// Callers must delete the result after any database that is using the
// result has been closed.
//
// Note: if you are using a custom comparator that ignores some parts
// of the keys being compared, you must not use NewBloomFilterPolicy()
// and must provide your own FilterPolicy that also ignores the
// corresponding parts of the keys.  For example, if the comparator
// ignores trailing spaces, it would be incorrect to use a
// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
// trailing spaces in keys.
//
// 返回一个新的 过滤策略, 该策略使用一个 布隆过滤器,
// 每个键的位数大约为指定的位数. bits_per_key 的一个好的值是 10,
// 它产生的过滤器误报率约为 1%.
//
// 调用者必须在 使用结果的任何数据库 关闭后 删除结果.
//
// 注意: 如果您使用 忽略正在比较的 key 的 某些部分 的 自定义比较器, 则不得使用
// NewBloomFilterPolicy() , 你必须提供自己的 FilterPolicy, 它 (需要) 也
// 忽略 key 的相应部分. 例如, 如果比较器 忽略 尾随空格, 则使用不忽略 key 中
// 尾随空格 的 FilterPolicy (如 NewBloomFilterPolicy) 是不正确的.
LEVELDB_EXPORT const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
