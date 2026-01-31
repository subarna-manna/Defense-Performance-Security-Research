// common/bloom_filter.h
#ifndef BLOOM_FILTER_H
#define BLOOM_FILTER_H

#include <bitset>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>

// Simple MurmurHash3 (64-bit) - from public domain implementations
uint64_t murmur_hash3(const void* key, size_t len, uint64_t seed = 0x9747b28c) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    uint64_t h = seed ^ len;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x5bd1e995;
        h ^= h >> 24;
    }
    return h;
}

class BloomFilter {
private:
    std::bitset<0> bits_;  // dynamic size later
    size_t size_bits_;
    size_t num_hashes_;

public:
    BloomFilter(size_t expected_items, double false_positive_rate) {
        size_bits_ = static_cast<size_t>(-std::log(false_positive_rate) * expected_items / (std::log(2) * std::log(2)));
        num_hashes_ = static_cast<size_t>(std::log(2) * size_bits_ / expected_items);
        bits_ = std::bitset<0>(size_bits_);  // placeholder - use dynamic bitset or vector<bool>
        // Better: use std::vector<bool> or boost::dynamic_bitset for large sizes
        // For simplicity: assume < 1e9 bits (125 MB)
    }

    void insert(const std::string& item) {
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t hash = murmur_hash3(item.data(), item.size(), i);
            size_t pos = hash % size_bits_;
            bits_[pos] = true;
        }
    }

    bool probably_contains(const std::string& item) const {
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t hash = murmur_hash3(item.data(), item.size(), i);
            size_t pos = hash % size_bits_;
            if (!bits_[pos]) return false;
        }
        return true;
    }

    void clear() {
        bits_.reset();
    }
};

#endif