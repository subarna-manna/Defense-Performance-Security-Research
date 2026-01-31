// data/fast_access/dedup_chunk_store.h

#ifndef DEDUP_CHUNK_STORE_H
#define DEDUP_CHUNK_STORE_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <leveldb/db.h>
#include <openssl/sha.h>
#include "config_loader.h"

class DedupChunkStore {
private:
    CountingBloomFilter* bloom_ = nullptr;
    BloomFilter* bloom_filter_ = nullptr;
    leveldb::DB* db_ = nullptr;                  // metadata: hash → ref_count + location
    std::string chunk_base_path_;                // /data/dedup/chunks/
    std::mutex mutex_;

    std::string get_chunk_path(const std::string& chunk_hash) const;
    // ... existing members ...

    // GC related
    std::atomic<bool> gc_running_{false};
    std::thread gc_thread_;
    std::condition_variable gc_cv_;
    std::mutex gc_mutex_;

    // Configurable (from g_config)
    std::chrono::hours gc_interval_{24};           // Run once per day
    std::chrono::hours gc_grace_period_{48};       // Keep unreferenced chunks for 2 days
    size_t gc_batch_size_{1000};                   // Process in batches to avoid long locks

    // Stats for metrics
    std::atomic<uint64_t> total_reclaimed_bytes_{0};
    std::atomic<uint64_t> total_deleted_chunks_{0};

    void gc_loop();
    void process_gc_batch(leveldb::Iterator* it, leveldb::WriteBatch& batch);
    void delete_chunk_file(const std::string& chunk_hash);

public:
    DedupChunkStore();
    ~DedupChunkStore();

    // Returns chunk_hash if already exists, or stores new and returns hash
    std::string StoreChunk(const std::vector<uint8_t>& chunk_data, bool& was_new);

    bool GetChunk(const std::string& chunk_hash, std::vector<uint8_t>& data);

    void IncrementRef(const std::string& chunk_hash);
    void DecrementRef(const std::string& chunk_hash);

    size_t GetRefCount(const std::string& chunk_hash) const;
    // ... existing public methods ...

    void StartGC();
    void StopGC();

    uint64_t GetReclaimedBytes() const { return total_reclaimed_bytes_.load(); }
    uint64_t GetDeletedChunks() const { return total_deleted_chunks_.load(); }
    bool probably_exists(const std::string& chunk_hash) const;

    // ... existing methods ...

    bool probably_exists(const std::string& chunk_hash) const;
    void increment_bloom(const std::string& chunk_hash);
    void decrement_bloom(const std::string& chunk_hash);

};

#endif



