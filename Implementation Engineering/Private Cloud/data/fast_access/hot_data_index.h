// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/fast_access/hot_data_index.h
// Hot Data Index
// Tracks frequently accessed mission data for promotion to fast storage

#ifndef HOT_DATA_INDEX_H
#define HOT_DATA_INDEX_H

#include <string>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include "config_loader.h"  // g_config

struct HotEntry {
    std::string bucket_key;           // "bucket/object-key"
    uint64_t access_count = 0;
    std::chrono::steady_clock::time_point last_access;
    double heat_score = 0.0;
    bool is_hot = false;              // cached flag
};

class HotDataIndex {
private:
    std::unordered_map<std::string, HotEntry> index_;
    std::mutex mutex_;

    // Priority queues for hot/cold candidates
    struct HotCompare {
        bool operator()(const HotEntry* a, const HotEntry* b) const {
            return a->heat_score < b->heat_score;
        }
    };
    std::priority_queue<HotEntry*, std::vector<HotEntry*>, HotCompare> hot_queue_;

    struct ColdCompare {
        bool operator()(const HotEntry* a, const HotEntry* b) const {
            return a->heat_score > b->heat_score;
        }
    };
    std::priority_queue<HotEntry*, std::vector<HotEntry*>, ColdCompare> cold_queue_;

    // Decay & maintenance
    std::atomic<bool> running_{true};
    std::thread decay_thread_;
    std::condition_variable cv_;
    std::mutex decay_mutex_;

    // Configurable thresholds (loaded from g_config)
    uint64_t min_hot_access_count_ = 100;
    double hot_threshold_score_ = 50.0;
    std::chrono::seconds decay_interval_{300};  // 5 min
    double decay_factor_ = 0.9;                 // multiply count by this each decay

    void decay_loop();
    void update_heat_score(HotEntry& entry);
    void promote_to_hot(const std::string& bucket_key);
    void demote_from_hot(const std::string& bucket_key);

public:
    HotDataIndex();
    ~HotDataIndex();

    void Start();
    void Stop();

    // Called on every access (Get/Put/Head)
    void RecordAccess(const std::string& bucket, const std::string& key);

    // Query status
    bool IsHot(const std::string& bucket, const std::string& key) const;
    size_t GetHotCount() const;
    double GetHeatScore(const std::string& bucket, const std::string& key) const;

    // For integration: get top N hot objects (e.g., for prefetch or replication priority)
    std::vector<std::string> GetTopHotObjects(size_t n) const;
};

#endif