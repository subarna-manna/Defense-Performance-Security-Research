#include "dedup_chunk_store.h"
#include <fstream>
#include <iomanip>
#include <sstream>

DedupChunkStore::DedupChunkStore() {
    chunk_base_path_ = g_config.storage_path + "/dedup/chunks";
    fs::create_directories(chunk_base_path_);

    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(options, g_config.storage_path + "/dedup_meta", &db_);
    if (!status.ok()) {
        throw std::runtime_error("Dedup LevelDB open failed: " + status.ToString());
    }

    // ... existing init ...

    if (g_config.enable_bloom_filter) {
        bloom_filter_ = new BloomFilter(g_config.bloom_expected_items,
                                        g_config.bloom_false_positive_rate);
        // Load existing chunks into Bloom (on startup)
        load_bloom_from_db();
    }


    // ... existing LevelDB + chunk path init ...

    if (g_config.enable_bloom_filter) {
        bloom_ = new CountingBloomFilter(
            g_config.bloom_expected_items,
            g_config.bloom_false_positive_rate,
            8  // 8-bit counters
        );

        // Populate from existing entries
        load_bloom_from_db();
    }

}

DedupChunkStore::~DedupChunkStore() {
    delete db_;
    delete bloom_;
}

std::string DedupChunkStore::get_chunk_path(const std::string& chunk_hash) const {
    return chunk_base_path_ + "/" + chunk_hash.substr(0, 2) + "/" + chunk_hash.substr(2);
}

std::string DedupChunkStore::StoreChunk(const std::vector<uint8_t>& chunk_data, bool& was_new) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(chunk_data.data(), chunk_data.size(), hash);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    std::string chunk_hash = ss.str();

    std::lock_guard<std::mutex> lock(mutex_);

    std::string value;
    leveldb::Status s = db_->Get(leveldb::ReadOptions(), chunk_hash, &value);

    if (s.ok()) {
        // Exists → increment ref count
        uint64_t ref = std::stoull(value) + 1;
        db_->Put(leveldb::WriteOptions(), chunk_hash, std::to_string(ref));
        was_new = false;
        return chunk_hash;
    }

    // New chunk → store encrypted
    std::vector<uint8_t> ciphertext, tag, iv;
    if (!encrypt_data(chunk_data, ciphertext, tag, iv)) {  // reuse your encrypt func
        return "";
    }

    std::string chunk_path = get_chunk_path(chunk_hash);
    fs::create_directories(fs::path(chunk_path).parent_path());

    std::ofstream ofs(chunk_path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(iv.data()), iv.size());
    ofs.write(reinterpret_cast<const char*>(tag.data()), tag.size());
    ofs.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    ofs.close();

    // Store ref count = 1
    db_->Put(leveldb::WriteOptions(), chunk_hash, "1");
    was_new = true;

    return chunk_hash;
}

// GetChunk, IncrementRef, DecrementRef implementations follow similar pattern...


#include <thread>
#include <chrono>
#include <filesystem>
#include <leveldb/write_batch.h>

// Assume these exist
extern void secure_log(const std::string& msg, int level = 0);
extern class MetricsCollector* metrics_;  // for exporting stats

void DedupChunkStore::StartGC() {
    if (gc_running_.exchange(true)) return;  // already running

    gc_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(gc_mutex_);
        while (gc_running_) {
            gc_loop();

            // Wait next interval or shutdown signal
            gc_cv_.wait_for(lock, gc_interval_, [this]{ return !gc_running_; });
        }
    });

    secure_log("[DEDUP_GC] Garbage collection thread started (interval: " +
               std::to_string(gc_interval_.count()) + " hours)", 1);
}

void DedupChunkStore::StopGC() {
    if (!gc_running_.exchange(false)) return;

    gc_cv_.notify_all();
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }

    secure_log("[DEDUP_GC] Garbage collection thread stopped", 1);
}

void DedupChunkStore::gc_loop() {
    secure_log("[DEDUP_GC] Starting garbage collection cycle", 1);

    leveldb::ReadOptions read_opts;
    read_opts.fill_cache = false;  // don't pollute cache

    leveldb::Iterator* it = db_->NewIterator(read_opts);
    leveldb::WriteBatch batch;
    size_t processed = 0;
    uint64_t reclaimed_this_cycle = 0;
    uint64_t deleted_this_cycle = 0;

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string hash = it->key().ToString();
        std::string value = it->value().ToString();

        uint64_t ref_count = 0;
        try {
            ref_count = std::stoull(value);
        } catch (...) {
            secure_log("[DEDUP_GC] Invalid ref count for chunk " + hash, 3);
            continue;
        }

        if (ref_count > 0) continue;

        // Chunk is unreferenced → check grace period
        // For simplicity: delete immediately (grace via delayed GC run)
        // Alternative: store deletion timestamp and check age

        std::string chunk_path = get_chunk_path(hash);
        uint64_t chunk_size = 0;
        if (fs::exists(chunk_path)) {
            chunk_size = fs::file_size(chunk_path);
            delete_chunk_file(hash);
            reclaimed_this_cycle += chunk_size;
            deleted_this_cycle++;
        }

        // Mark for deletion in batch
        batch.Delete(hash);

        processed++;
        if (processed % gc_batch_size_ == 0) {
            leveldb::Status s = db_->Write(leveldb::WriteOptions(), &batch);
            if (!s.ok()) {
                secure_log("[DEDUP_GC] Batch write failed: " + s.ToString(), 3);
            }
            batch.Clear();
        }
    }

    // Final batch
    if (!batch.IsEmpty()) {
        db_->Write(leveldb::WriteOptions(), &batch);
    }

    delete it;

    total_reclaimed_bytes_ += reclaimed_this_cycle;
    total_deleted_chunks_ += deleted_this_cycle;

    // Export to metrics
    if (metrics_) {
        metrics_->dedup_reclaimed_bytes = total_reclaimed_bytes_.load();
        metrics_->dedup_deleted_chunks = total_deleted_chunks_.load();
    }

    secure_log("[DEDUP_GC] Cycle complete: deleted " + std::to_string(deleted_this_cycle) +
               " chunks, reclaimed " + std::to_string(reclaimed_this_cycle / 1024 / 1024) +
               " MiB", 1);
}

void DedupChunkStore::delete_chunk_file(const std::string& chunk_hash) {
    std::string path = get_chunk_path(chunk_hash);
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            secure_log("[DEDUP_GC] Deleted chunk file: " + chunk_hash, 2);
        } catch (const std::exception& e) {
            secure_log("[DEDUP_GC] Failed to delete chunk file " + chunk_hash + ": " + e.what(), 3);
        }
    }
}

void DedupChunkStore::DecrementRef(const std::string& chunk_hash) {
    // ... decrease count ...
    if (new_ref_count == 0) {
        // Optional: immediate delete (risky) or just wait for GC cycle
        secure_log("[DEDUP_GC] Ref count reached 0 for " + chunk_hash + " - scheduled for GC", 2);
    }
}



// 3. Integration & Lifecycle
// In DedupChunkStore constructor:
// C++StartGC();  // auto-start on creation
// In destructor:
// C++StopGC();
// When DecrementRef drops to 0 (optional immediate trigger):


void DedupChunkStore::load_bloom_from_db() {
    if (!bloom_filter_) return;

    leveldb::Iterator* it = db_->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string hash = it->key().ToString();
        std::string value = it->value().ToString();
        uint64_t ref = std::stoull(value);
        if (ref > 0) {
            bloom_filter_->insert(hash);
        }
    }
    delete it;
    secure_log("[DEDUP] Bloom filter loaded with existing chunks", 1);
}

bool DedupChunkStore::probably_exists(const std::string& chunk_hash) const {
    if (!bloom_filter_) return true;  // fallback: assume exists
    return bloom_filter_->probably_contains(chunk_hash);
}

// Optimized StoreChunk
std::string DedupChunkStore::StoreChunk(const std::vector<uint8_t>& chunk_data, bool& was_new) {
    unsigned char hash_bytes[SHA256_DIGEST_LENGTH];
    SHA256(chunk_data.data(), chunk_data.size(), hash_bytes);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash_bytes[i]);
    }
    std::string chunk_hash = ss.str();

    // Bloom quick check
    if (bloom_filter_ && !bloom_filter_->probably_contains(chunk_hash)) {
        // Definitely new → insert
        was_new = true;
        bloom_filter_->insert(chunk_hash);
        // proceed to store
    } else {
        // Possible exists → check LevelDB
        std::string value;
        leveldb::Status s = db_->Get(leveldb::ReadOptions(), chunk_hash, &value);
        if (s.ok() && std::stoull(value) > 0) {
            was_new = false;
            // Increment ref (existing code)
            return chunk_hash;
        }
        was_new = true;
        if (bloom_filter_) bloom_filter_->insert(chunk_hash);
    }

    // ... rest of store logic (encrypt, write file, set ref=1) ...
}

// Similarly optimize GetChunk
bool DedupChunkStore::GetChunk(const std::string& chunk_hash, std::vector<uint8_t>& data) {
    if (bloom_filter_ && !bloom_filter_->probably_contains(chunk_hash)) {
        return false;  // definitely not exists
    }

    // ... proceed to LevelDB check and file read ...
}






