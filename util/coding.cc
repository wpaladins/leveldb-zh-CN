// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

namespace leveldb {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint32(char* dst, uint32_t v) {
  // Operate on characters as unsigneds
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;
  // v < 128 时
  //         给 *ptr 直接赋值为 v, 并将 ptr++
  // 2 ^ 7 <= v < 2 ^ 14 时
  //         给 *ptr 赋值为 v | B, 并将 ptr++ (即 将 v 的 第 8 位 置为1)
  //         给 *ptr 赋值为 v >> 7 (v 除去低7位), 并将 ptr++
  //
  // 因此, varint32 中
  //         第一个字节 和 最后一个字节 的值 的确定 需要 一个 位运算 即可
  //         (分别为 置位 移位); 其他字节 的值 的确定 需要 两个位运算 (置位 +
  //         移位)
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;
    *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, ptr - buf);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  static const int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  // 与 EncodeVarint32 的做法不同
  // 如果 剩余的数 v 不能用单字节表示 (单字节 能表示的数的范围 [0..127])
  //         先存 低 7 位 至一个字节 (字节首位 置为 1)
  //         对 v 本身进行移位修改
  // 剩余的数 v 能用单字节表示时 (后)
  //         直接存入 (无需置位, 字节首位 必然为 0)
  //
  // 因此, varint64 中
  //         最后一个字节 的值 的确定 无需 位运算
  //         其他字节 的值 确定 的同时 需要 两个位运算 (置位 + 移位)
  while (v >= B) {
    *(ptr++) = v | B;
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, ptr - buf);
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, value.size());
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t v) {
  // 至少 1 字节
  int len = 1;
  // 如果 (除去低位后的数) 7 位装不下, 则增加 1 字节
  // 7 位 (128) 的原因: 每个 字节的第一位标识 是否为 varint 的最后一个字节
  while (v >= 128) {
    v >>= 7;
    len++;
  }
  return len;
}

// 将存储在 字符数组 [p..limit-1] 中的 varint32 转换为 uint32
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  // shift 移位长
  // 可能取值 0 7 14 21 28
  // shift 为 28 时, 得到的 byte 必然 小于 16 (仅 4 位)
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    // byte 为什么要存入 uint32 而不是 uint8 ?
    // 因为 接下来要对 byte 向左移位
    // 因此 可以得知 先得到的数据 为 uint32 的 低位数据
    //      因为 这里的做法是 给 新得到的数据 做 大位移 后 | 到结果中
    //      如果 要让 先得到的数据 为 uint32 的 高位数据
    //      可以 先对结果做 大位移 后 | 上 新得到的数据
    //      (这么做 shift 的 变化规则 可以不变)
    // 注意 这里每次从 字符串 [p..limit-1] 中取 8 位
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    // & 128 来检查 (原始数据的) 第一位 是否为 1
    if (byte & 128) {
      // More bytes are present
      // & 127 来将 第一位 置 0
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    // 对 *input 重新赋值 以删除此次获取的 varint32
    *input = Slice(q, limit - q);
    return true;
  }
}

// 逻辑同 GetVarint32PtrFallback
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

// 逻辑同 GetVarint32
bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  // 从 input 中获取长度 len, 并 验证合法性
  // 注意 GetVarint32 会修改 *input
  // 因此 合法性验证 时的 input->size() 已经不包含 len 值的长度
  if (GetVarint32(input, &len) && input->size() >= len) {
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb
