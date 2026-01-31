// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/replication/sync_engine.h
// Replication Engine
// Ensures cross-region data consistency (eventual)

#ifndef SYNC_ENGINE_H
#define SYNC_ENGINE_H

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>
#include "config_loader.h"     // g_config
#include "storage_manager.h"   // For object access


#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <leveldb/status.h>



namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

struct ReplicationTask {
    std::string bucket;
    std::string key;
    std::string version_id;      // optional
    std::string etag;
    std::chrono::system_clock::time_point queued_time;
};

class ReplicationEngine {
private:
    StorageManager& storage_;                  // Reference to local storage
    std::queue<ReplicationTask> task_queue_;
    std::mutex queue_mutex_;
    std::atomic<bool> running_{true};
    std::vector<std::thread> worker_threads_;

    std::vector<std::pair<std::string, std::string>> replica_endpoints_; // region: https://remote:port

    // HTTP client for replication push
    net::io_context ioc_;
    std::unique_ptr<net::ssl::context> ssl_ctx_; // For mTLS to remotes

    // Persistent queue instead of std::queue
    leveldb::DB* queue_db_ = nullptr;
    std::string queue_db_path_;           // e.g. g_config.storage_path + "/replication_queue"

    std::mutex db_mutex_;                 // protect DB operations

    // In-memory cache of pending tasks (for faster peek/pop)
    std::priority_queue<QueuedTask, std::vector<QueuedTask>, std::greater<QueuedTask>> task_cache_;
    std::mutex cache_mutex_;

    struct QueuedTask {
        uint64_t sequence;               // monotonic increasing ID for ordering
        ReplicationTask task;
        bool operator>(const QueuedTask& other) const { return sequence > other.sequence; }
    };

    uint64_t next_sequence_ = 0;         // persisted in DB under key "meta:next_seq"

    // Circuit breakers and other members remain the same...


    void worker_loop();
    bool replicate_to_endpoint(const ReplicationTask& task, const std::string& endpoint);
    bool push_object_via_http(const std::string& endpoint, const std::string& bucket,
                              const std::string& key, const std::vector<uint8_t>& data,
                              const std::string& content_type, const std::string& etag);

    // Placeholder for secure_logger integration
    void log_replication_event(const std::string& msg, bool success);


    bool open_queue_db();
    bool load_next_sequence();
    bool enqueue_task(const ReplicationTask& task);
    bool dequeue_task(ReplicationTask& task);
    bool remove_task(uint64_t sequence);
    void persist_next_sequence();

    

public:
    explicit ReplicationEngine(StorageManager& storage);
    ~ReplicationEngine();

    void Start(int num_workers = 4);
    void Stop();

    // Called by StorageManager after successful write
    void QueueReplication(const std::string& bucket, const std::string& key,
                          const std::string& etag, const std::string& version_id = "");

    // Manual trigger for resync (e.g., on region recovery)
    void TriggerFullResync(const std::string& bucket = "");
};

#endif


// Add these constants / members
private:
    static constexpr int    MAX_RETRIES_PER_ENDPOINT = 5;
    static constexpr int    BASE_BACKOFF_SECONDS     = 2;
    static constexpr double JITTER_FACTOR            = 0.5;   // ±50% random variation

    struct EndpointStatus {
        int consecutive_failures = 0;
        std::chrono::steady_clock::time_point next_attempt_time;
    };

    std::map<std::string, EndpointStatus> endpoint_status_;  // endpoint_url → retry state



// Add these enums and struct
enum class CircuitState {
    Closed,
    Open,
    HalfOpen
};

struct CircuitBreaker {
    CircuitState state = CircuitState::Closed;
    int failure_count = 0;
    std::chrono::steady_clock::time_point last_failure_time;
    std::chrono::steady_clock::time_point next_probe_time;

    // Configurable thresholds
    static constexpr int FAILURE_THRESHOLD = 5;
    static constexpr auto OPEN_TIMEOUT = std::chrono::minutes(5);
    static constexpr int HALF_OPEN_MAX_FAILURES = 1;
};

// In ReplicationEngine class
private:
    std::map<std::string, CircuitBreaker> circuit_breakers_;  // endpoint → its breaker












