// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

// 一个块的大小, 一般情况下, 用该大小来调用 new 获得新内存
static const int kBlockSize = 4096;

// 默认为空
Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}

/**
 * 进入该函数, 说明当前 分配状态 已经不足以满足分配需求
 */
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    // 为了避免浪费剩余字节
    // 在 目标 大于块大小的 1/4 时
    // 独立为其分配内存
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  // 浪费当前块的剩余字节
  // 分配一个新的块
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  // 满足分配请求, 更新 分配状态
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  // 通过 指针大小 确定要对齐到的字节数 (最小为 8)
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  // 使用 位运算 取模
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1);
  // 填充多少
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  // 总需要多少
  size_t needed = bytes + slop;
  char* result;
  // 当前 分配状态 够用时
  if (needed <= alloc_bytes_remaining_) {
    // 注意: result 的值表明, 所谓对齐是将 result 的起始地址对齐 (即 填充填到了
    // result 前面)
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returned aligned memory
    // AllocateFallback 总是返回一个对齐的内存
    // 为什么?
    // 因为 在当前分配状态不够用时 AllocateFallback 会调用
    // AllocateNewBlock(kBlockSize) , 进而使用 new 分配一块内存
    result = AllocateFallback(bytes);
  }
  // 检查是否达到对齐的目的
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

/**
 * 使用 new 分配一个新的 block (char数组)
 */
char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace leveldb
