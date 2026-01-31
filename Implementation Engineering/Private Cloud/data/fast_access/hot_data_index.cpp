// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/fast_access/hot_data_index.cpp

#include "hot_data_index.h"
#include <algorithm>
#include <iostream>

HotDataIndex::HotDataIndex() {
    // Load thresholds from config (fallback defaults)
    min_hot_access_count_ = g_config.hot_min_access_count > 0 ? g_config.hot_min_access_count : 100;
    hot_threshold_score_ = g_config.hot_threshold_score > 0 ? g_config.hot_threshold_score : 50.0;
    decay_interval_ = std::chrono::seconds(g_config.hot_decay_interval_sec > 0 ? g_config.hot_decay_interval_sec : 300);
    decay_factor_ = g_config.hot_decay_factor > 0 && g_config.hot_decay_factor <= 1.0 ? g_config.hot_decay_factor : 0.9;

    std::cout << "HotDataIndex initialized: min_access=" << min_hot_access_count_
              << ", threshold=" << hot_threshold_score_
              << ", decay=" << decay_factor_ << std::endl;
}

HotDataIndex::~HotDataIndex() {
    Stop();
}

void HotDataIndex::Start() {
    decay_thread_ = std::thread(&HotDataIndex::decay_loop, this);
}

void HotDataIndex::Stop() {
    running_ = false;
    cv_.notify_all();
    if (decay_thread_.joinable()) decay_thread_.join();
}

void HotDataIndex::RecordAccess(const std::string& bucket, const std::string& key) {
    std::string bucket_key = bucket + "/" + key;

    std::lock_guard<std::mutex> lock(mutex_);

    auto& entry = index_[bucket_key];
    if (entry.bucket_key.empty()) {
        entry.bucket_key = bucket_key;
    }

    entry.access_count++;
    entry.last_access = std::chrono::steady_clock::now();
    update_heat_score(entry);

    // Check promotion
    if (!entry.is_hot && (entry.access_count >= min_hot_access_count_ || entry.heat_score >= hot_threshold_score_)) {
        promote_to_hot(bucket_key);
    }

    // Update queues (simple push; in prod use heapify or sorted set)
    hot_queue_.push(&entry);
    cold_queue_.push(&entry);
}

bool HotDataIndex::IsHot(const std::string& bucket, const std::string& key) const {
    std::string bucket_key = bucket + "/" + key;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(bucket_key);
    return it != index_.end() && it->second.is_hot;
}

double HotDataIndex::GetHeatScore(const std::string& bucket, const std::string& key) const {
    std::string bucket_key = bucket + "/" + key;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(bucket_key);
    return it != index_.end() ? it->second.heat_score : 0.0;
}

size_t HotDataIndex::GetHotCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [k, e] : index_) {
        if (e.is_hot) ++count;
    }
    return count;
}

std::vector<std::string> HotDataIndex::GetTopHotObjects(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> top;
    auto temp = hot_queue_;
    while (top.size() < n && !temp.empty()) {
        top.push_back(temp.top()->bucket_key);
        temp.pop();
    }
    return top;
}

void HotDataIndex::update_heat_score(HotEntry& entry) {
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration<double>(now - entry.last_access).count() / 3600.0;  // hours
    entry.heat_score = entry.access_count * 1.0 + (age < 24 ? (24 - age) * 10 : 0);  // recency boost
}

void HotDataIndex::promote_to_hot(const std::string& bucket_key) {
    auto& entry = index_[bucket_key];
    entry.is_hot = true;
    // Integrate: tell LRU Cache to pin/promote this object
    // lru_cache->Promote(bucket_key);
    // Or move file to fast SSD path if using tiered storage
    log_hot_event("Promoted to hot: " + bucket_key);
}

void HotDataIndex::demote_from_hot(const std::string& bucket_key) {
    auto it = index_.find(bucket_key);
    if (it == index_.end() || !it->second.is_hot) return;
    it->second.is_hot = false;
    // lru_cache->Demote(bucket_key);
    log_hot_event("Demoted from hot: " + bucket_key);
}

void HotDataIndex::decay_loop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(decay_mutex_);
        if (cv_.wait_for(lock, decay_interval_, [this]{ return !running_; })) break;

        std::lock_guard<std::mutex> idx_lock(mutex_);

        for (auto& [key, entry] : index_) {
            if (entry.access_count > 0) {
                entry.access_count = static_cast<uint64_t>(entry.access_count * decay_factor_);
                update_heat_score(entry);

                // Check demotion
                if (entry.is_hot && entry.heat_score < hot_threshold_score_ / 2) {
                    demote_from_hot(key);
                }
            }
        }

        // Trim queues if too large (prevent memory bloat)
        // ... optional
    }
}

// Placeholder logger (integrate with secure_logger.cpp)
void log_hot_event(const std::string& msg) {
    std::cout << "[HOT_INDEX] " << msg << std::endl;
    // secure_log("HOT_INDEX: " + msg);
}

void HotDataIndex::promote_to_hot(const std::string& bucket_key) {
    auto [bucket, key] = split_bucket_key(bucket_key);  // helper function
    extern StorageManager* storage_manager_;  // or injected
    if (storage_manager_->PromoteObject(bucket, key)) {
        auto& entry = index_[bucket_key];
        entry.is_hot = true;
        log_hot_event("Promoted & moved to Hot tier: " + bucket_key);
    }
}


void HotDataIndex::demote_from_hot(const std::string& bucket_key) {
    auto [bucket, key] = split_bucket_key(bucket_key);
    extern StorageManager* storage_manager_;
    if (storage_manager_->DemoteObject(bucket, key)) {
        auto& entry = index_[bucket_key];
        entry.is_hot = false;
        log_hot_event("Demoted & moved to Warm/Cold tier: " + bucket_key);
    }
}



void HotDataIndex::tier_maintenance_loop() {

    if (!entry.is_hot && entry.heat_score < g_config.remote_cold_heat_threshold &&
        (now - entry.last_access) > std::chrono::days(g_config.remote_cold_demote_age_days)) {
        extern StorageManager* storage_manager_;
        storage_manager_->DemoteToRemoteCold(bucket, key);
    }
    
    while (running_) {
        std::this_thread::sleep_for(std::chrono::minutes(15));

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, entry] : index_) {
            if (entry.is_hot && entry.heat_score < hot_threshold_score_ / 2) {
                demote_from_hot(key);
            } else if (!entry.is_hot && entry.heat_score > hot_threshold_score_ * 1.5) {
                promote_to_hot(key);
            }
        }
    }
}


