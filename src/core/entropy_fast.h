// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace hypercine { namespace standalone { namespace detail {
// MSB-first VLC. At entry/exit pending_ is in [0,31]; appending <=32
// unsigned bits needs at most 63 bits. No shift by 64 or signed shift occurs.
// Completed words are emitted in wire order, independently of host endianness.
class EntropyBits {
    std::size_t capacity_;
    uint64_t cache_=0;
    unsigned pending_=0;
public:
    std::vector<uint8_t> bytes;
    EntropyBits(std::size_t capacity,std::size_t reserve) : capacity_(capacity) {
        bytes.reserve(reserve<capacity ? reserve:capacity);
    }
    void Put(uint32_t code,unsigned size) {
        if(size<1 || size>32 || (size<32 && (code>>size)))
            throw std::invalid_argument("invalid VLC codeword");
        const auto count=pending_+size;
        // Check before changing the cache; account for the partial final byte.
        if((count+7)/8>capacity_-bytes.size())
            throw std::length_error("entropy capacity exceeded");
        cache_=(cache_<<size)|code;
        pending_=count;
        if(pending_>=32) {
            pending_-=32;
            const auto word=static_cast<uint32_t>(cache_>>pending_);
            bytes.push_back(static_cast<uint8_t>(word>>24));
            bytes.push_back(static_cast<uint8_t>(word>>16));
            bytes.push_back(static_cast<uint8_t>(word>>8));
            bytes.push_back(static_cast<uint8_t>(word));
        }
    }
    void Finish() {if(pending_)Put(0,32-pending_);}
};
}}}
