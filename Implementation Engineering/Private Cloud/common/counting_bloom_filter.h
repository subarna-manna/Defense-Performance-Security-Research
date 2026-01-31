#ifndef COUNTING_BLOOM_FILTER_H
#define COUNTING_BLOOM_FILTER_H

#include <vector>
#include <cstdint>
#include <string>
#include <cmath>
#include <limits>

// Simple MurmurHash3 64-bit (public domain)
inline uint64_t murmur3_64(const void* key, size_t len, uint64_t seed = 0x9747b28c) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    uint64_t h = seed ^ len;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x5bd1e995;
        h ^= h >> 24;
    }
    return h;
}

class CountingBloomFilter {
private:
    std::vector<uint8_t> counters_;   // 8-bit counters (0–255)
    size_t size_;                     // number of counters
    size_t num_hashes_;               // number of hash functions
    uint8_t max_count_ = 255;         // saturation limit

public:
    CountingBloomFilter(size_t expected_items, double false_positive_rate, uint8_t bits_per_counter = 8) {
        double ln2 = std::log(2.0);
        size_ = static_cast<size_t>(-std::log(false_positive_rate) * expected_items / (ln2 * ln2));
        num_hashes_ = static_cast<size_t>(ln2 * size_ / expected_items);

        // Round up to power of 2 or nice number for speed (optional)
        size_ = ((size_ + 63) / 64) * 64;

        counters_.assign(size_, 0);
    }

    void insert(const std::string& item) {
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t h = murmur3_64(item.data(), item.size(), i);
            size_t pos = h % size_;
            if (counters_[pos] < max_count_) {
                counters_[pos]++;
            }
        }
    }

    void remove(const std::string& item) {
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t h = murmur3_64(item.data(), item.size(), i);
            size_t pos = h % size_;
            if (counters_[pos] > 0) {
                counters_[pos]--;
            }
        }
    }

    bool probably_contains(const std::string& item) const {
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t h = murmur3_64(item.data(), item.size(), i);
            size_t pos = h % size_;
            if (counters_[pos] == 0) {
                return false;
            }
        }
        return true;
    }

    void clear() {
        std::fill(counters_.begin(), counters_.end(), 0);
    }

    size_t estimated_count(const std::string& item) const {
        size_t min_val = std::numeric_limits<size_t>::max();
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint64_t h = murmur3_64(item.data(), item.size(), i);
            size_t pos = h % size_;
            min_val = std::min<size_t>(min_val, counters_[pos]);
        }
        return min_val;
    }

    size_t memory_usage_bytes() const {
        return counters_.size() * sizeof(uint8_t);
    }
};

#endif