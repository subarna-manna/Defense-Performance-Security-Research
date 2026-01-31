// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/replication/sync_engine.cpp

#include "sync_engine.h"
#include <openssl/evp.h>   // For any extra crypto if needed
#include <iostream>
#include <sstream>
#include <random>
#include <iomanip>
// ... existing includes ...
#include <zlib.h>  // for compression (link -lz)
#include <random>


secure_log("Compressed object " + bucket + "/" + key + " with level " + std::to_string(comp_level) +
           " for tier " + std::to_string(static_cast<int>(target_tier)));

// In ReplicationEngine class add:
private:
    std::chrono::seconds backoff_base_{30};  // start 30s
    int max_backoff_attempts_{5};

    bool compress_data(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) const {
        z_stream strm{};
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;

        if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) return false;

        strm.avail_in = input.size();
        strm.next_in = const_cast<Bytef*>(input.data());

        output.resize(compressBound(input.size()));
        strm.avail_out = output.size();
        strm.next_out = output.data();

        int ret = deflate(&strm, Z_FINISH);
        deflateEnd(&strm);

        if (ret != Z_STREAM_END) return false;

        output.resize(output.size() - strm.avail_out);
        return true;
    }

    bool fetch_remote_etag(const std::string& endpoint, const std::string& bucket,
                           const std::string& key, std::string& remote_etag) {
        try {
            // Parse endpoint as before (host, port)
            // ... (reuse code from push_object_via_http for host/port)

            net::io_context ioc;
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(host, port);

            beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx_);
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(net::ssl::stream_base::client);

            http::request<http::empty_body> head_req{http::verb::head, "/" + bucket + "/" + key, 11};
            head_req.set(http::field::host, host);
            head_req.set(http::field::user_agent, "Defense Replication Engine");

            http::write(stream, head_req);

            beast::flat_buffer buffer;
            http::response<http::empty_body> head_res;
            http::read(stream, buffer, head_res);

            beast::error_code ec;
            stream.shutdown(ec);

            if (head_res.result() != http::status::ok) return false;

            remote_etag = head_res["ETag"].to_string();
            // Clean quotes if present (S3 style)
            if (!remote_etag.empty() && remote_etag.front() == '"' && remote_etag.back() == '"') {
                remote_etag = remote_etag.substr(1, remote_etag.size() - 2);
            }

            return true;
        } catch (...) {
            return false;
        }
    }

// Update replicate_to_endpoint



ReplicationEngine::ReplicationEngine(StorageManager& storage)
    : storage_(storage), ssl_ctx_(std::make_unique<net::ssl::context>(net::ssl::context::tls_client)) {
    // Load mTLS client config (from config_loader or mutual_tls.rs via FFI if Rust)
    // Example: ssl_ctx_->load_verify_file(g_config.ca_cert_path);
    // ssl_ctx_->use_certificate_file(g_config.server_cert_path, net::ssl::context::pem);
    // ssl_ctx_->use_private_key_file(g_config.server_key_path, net::ssl::context::pem);

    // Populate replica endpoints from config
    for (const auto& region : g_config.replica_regions) {
        // Assume format "region-name:https://remote-host:port"
        size_t colon = region.find(':');
        if (colon != std::string::npos) {
            std::string ep = region.substr(colon + 1);
            replica_endpoints_.emplace_back(region.substr(0, colon), ep);
        }
    }

    if (replica_endpoints_.empty()) {
        std::cerr << "Warning: No replica regions configured.\n";
    }
}

ReplicationEngine::~ReplicationEngine() {
    Stop();
}

void ReplicationEngine::Start(int num_workers) {
    for (int i = 0; i < num_workers; ++i) {
        worker_threads_.emplace_back(&ReplicationEngine::worker_loop, this);
    }
}

void ReplicationEngine::Stop() {
    running_ = false;
    for (auto& t : worker_threads_) {
        if (t.joinable()) t.join();
    }
}

void ReplicationEngine::QueueReplication(const std::string& bucket, const std::string& key,
                                         const std::string& etag, const std::string& version_id) {
    ReplicationTask task{bucket, key, version_id, etag, std::chrono::system_clock::now()};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(std::move(task));
    }
    log_replication_event("Queued replication for " + bucket + "/" + key, true);
}

void ReplicationEngine::worker_loop() {
    int backoff_attempts = 0;
    std::random_device rd;
    std::mt19937 gen(rd());

    while (running_) {
        ReplicationTask task;
        bool has_task = false;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
                has_task = true;
            }
        }

        if (!has_task) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        bool success = true;
        for (const auto& [region, endpoint] : replica_endpoints_) {
            if (!replicate_to_endpoint(task, endpoint)) {
                success = false;
                // Exponential backoff retry queue (simplified: re-queue)
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    task_queue_.push(task);
                }
                break;
            }
        }

        log_replication_event("Replication " + (success ? "succeeded" : "failed/requeued") +
                              " for " + task.bucket + "/" + task.key, success);

        // Optional: notify consistency_checker
        // consistency_checker->Validate(task.bucket, task.key, task.etag);

        bool success = true;
        for (const auto& [region, endpoint] : replica_endpoints_) {
            if (!replicate_to_endpoint(task, endpoint)) {
                success = false;
                break;
            }
        }



        // Update worker_loop for backoff

        if (success) {
            backoff_attempts = 0;
            log_replication_event("Replication succeeded for " + task.bucket + "/" + task.key, true);
        } else {
            backoff_attempts++;
            if (backoff_attempts > max_backoff_attempts_) {
                log_replication_event("Max backoff reached - dropping task " + task.key, false);
                continue;
            }
            // Exponential backoff + jitter
            std::uniform_int_distribution<> dist(0, 30);
            auto delay = backoff_base_ * (1 << (backoff_attempts - 1)) + std::chrono::seconds(dist(gen));
            log_replication_event("Replication failed - backoff " + std::to_string(delay.count()) + "s for " + task.key, false);
            std::this_thread::sleep_for(delay);

            // Requeue
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                task_queue_.push(task);
            }
        }

        bool all_success = true;

        for (const auto& [region, endpoint] : replica_endpoints_) {
            if (!replicate_to_endpoint(task, endpoint)) {
                all_success = false;
                // Do NOT break – try all endpoints even if one fails
                // This way partial replication is possible
            }
        }
        
        if (all_success) {
            log_replication_event("Full replication succeeded for " + task.bucket + "/" + task.key, true);
        } else {
            // At least one endpoint failed after retries → requeue whole task
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                task_queue_.push(task);
            }
            log_replication_event("Partial or full replication failure - task requeued", false);
        }



        // Updated worker_loop – Partial success handling

        bool any_success = false;
        bool all_failed = true;

        for (const auto& [region, endpoint] : replica_endpoints_) {
            bool this_success = replicate_to_endpoint(task, endpoint);
            if (this_success) {
                any_success = true;
            }
            if (this_success) {
                all_failed = false;
            }
        }

        if (any_success) {
            log_replication_event("Partial replication success for " + task.bucket + "/" + task.key, true);
        }

        if (all_failed) {
            // Only requeue if **all** endpoints failed
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                task_queue_.push(task);
            }
            log_replication_event("All endpoints failed - task requeued", false);
        } else {
            // At least one succeeded → consider task progressed (don't requeue immediately)
            // You can add delayed requeue logic for failed endpoints if desired
        }


    }
}








bool ReplicationEngine::replicate_to_endpoint(const ReplicationTask& task,
    const std::string& endpoint) {
    std::vector<uint8_t> data;
    StorageManager::ObjectMetadata meta;
    if (!storage_.GetObject(task.bucket, task.key, data, meta)) {
    log_replication_event("Failed to read local object: " + task.key, false);
    return false;
    }

    // Delta: Check remote ETag via HEAD
    std::string remote_etag;
    if (fetch_remote_etag(endpoint, task.bucket, task.key, remote_etag)) {
    if (remote_etag == meta.etag) {
    log_replication_event("Remote ETag matches - skipping replication for " + task.key, true);
    return true;
    }
    } else {
    log_replication_event("HEAD failed - assuming change and pushing full", false);
    }

    // Optional: Compress plaintext before encryption (but since encrypted in Put, compress here for transfer)
    std::vector<uint8_t> compressed;
    bool use_compression = data.size() > 1024 * 1024;  // >1MB
    if (use_compression && compress_data(data, compressed)) {
    // Send compressed, add header Content-Encoding: deflate
    // But for simplicity, send full uncompressed (or implement remote decompress)
    // Here: skip compression for now to avoid remote changes
    }

    // Push full (as before)
    bool success = push_object_via_http(endpoint, task.bucket, task.key, data,
    meta.content_type, meta.etag);

    return success;
}

bool ReplicationEngine::replicate_to_endpoint(const ReplicationTask& task,
    const std::string& endpoint) {
    auto& status = endpoint_status_[endpoint];

    // Check if we should wait before next attempt
    auto now = std::chrono::steady_clock::now();
    if (now < status.next_attempt_time) {
    std::this_thread::sleep_until(status.next_attempt_time);
    }

    std::vector<uint8_t> data;
    StorageManager::ObjectMetadata meta;
    if (!storage_.GetObject(task.bucket, task.key, data, meta)) {
    log_replication_event("Failed to read local object for replication: " + task.key, false);
    return false;
    }

    // Delta check (HEAD request) - unchanged from previous
    std::string remote_etag;
    bool has_remote_etag = fetch_remote_etag(endpoint, task.bucket, task.key, remote_etag);

    if (has_remote_etag && remote_etag == meta.etag) {
    log_replication_event("Remote ETag matches - skipping replication for " + task.key +
    " to " + endpoint, true);
    status.consecutive_failures = 0;  // reset on success/skip
    status.next_attempt_time = now;   // no delay needed
    return true;
    }

    // Attempt replication with retry loop
    bool success = false;
    for (int attempt = 1; attempt <= MAX_RETRIES_PER_ENDPOINT; ++attempt) {
    log_replication_event("Replicating " + task.bucket + "/" + task.key +
    " to " + endpoint + " (attempt " + std::to_string(attempt) +
    "/" + std::to_string(MAX_RETRIES_PER_ENDPOINT) + ")", true);

    success = push_object_via_http(endpoint, task.bucket, task.key, data,
    meta.content_type, meta.etag);

    if (success) {
    status.consecutive_failures = 0;
    status.next_attempt_time = now;
    log_replication_event("Replication succeeded to " + endpoint, true);
    break;
    }

    // Failure → exponential backoff + jitter
    status.consecutive_failures = attempt;

    int backoff_seconds = BASE_BACKOFF_SECONDS * (1 << (attempt - 1));

    // Add jitter: ± JITTER_FACTOR * backoff
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> jitter_dist(-JITTER_FACTOR, JITTER_FACTOR);
    double jitter = jitter_dist(gen);
    int adjusted_backoff = static_cast<int>(backoff_seconds * (1.0 + jitter));

    // Cap maximum delay (e.g. 10 minutes)
    adjusted_backoff = std::min(adjusted_backoff, 600);

    status.next_attempt_time = now + std::chrono::seconds(adjusted_backoff);

    log_replication_event("Replication failed to " + endpoint +
    " - retrying in " + std::to_string(adjusted_backoff) +
    " seconds (attempt " + std::to_string(attempt) + ")", false);

    if (attempt < MAX_RETRIES_PER_ENDPOINT) {
    std::this_thread::sleep_for(std::chrono::seconds(adjusted_backoff));
    }
    }

    if (!success) {
    log_replication_event("Max retries exceeded for " + endpoint +
    " - will requeue task later", false);
    }

    return success;
}

bool ReplicationEngine::replicate_to_endpoint(const ReplicationTask& task,
    const std::string& endpoint) {
    // Circuit breaker check
    if (!should_attempt_replication(endpoint)) {
    return false;  // skip this endpoint, but don't count as failure
    }

    std::vector<uint8_t> data;
    StorageManager::ObjectMetadata meta;
    if (!storage_.GetObject(task.bucket, task.key, data, meta)) {
    log_replication_event("Failed to read object for replication: " + task.key, false);
    record_failure(endpoint);
    return false;
    }

    // Delta check (HEAD) - if matches, treat as success
    std::string remote_etag;
    bool has_remote_etag = fetch_remote_etag(endpoint, task.bucket, task.key, remote_etag);

    if (has_remote_etag && remote_etag == meta.etag) {
    log_replication_event("Remote ETag matches - skipping replication for " + task.key +
    " to " + endpoint, true);
    record_success(endpoint);
    return true;
    }

    // Attempt actual replication
    bool success = push_object_via_http(endpoint, task.bucket, task.key, data,
    meta.content_type, meta.etag);

    if (success) {
    record_success(endpoint);
    log_replication_event("Replication succeeded to " + endpoint, true);
    } else {
    record_failure(endpoint);
    log_replication_event("Replication failed to " + endpoint, false);
    }

    return success;
}







bool ReplicationEngine::push_object_via_http(const std::string& endpoint_url,
                                             const std::string& bucket,
                                             const std::string& key,
                                             const std::vector<uint8_t>& data,
                                             const std::string& content_type,
                                             const std::string& etag) {
    try {
        // Parse endpoint_url[](https://host:port)
        std::string host, port = "443";
        size_t proto_pos = endpoint_url.find("://");
        if (proto_pos == std::string::npos) return false;
        size_t slash_pos = endpoint_url.find('/', proto_pos + 3);
        std::string authority = (slash_pos != std::string::npos)
            ? endpoint_url.substr(proto_pos + 3, slash_pos - proto_pos - 3)
            : endpoint_url.substr(proto_pos + 3);

        size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }

        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host, port);

        beast::ssl_stream<beast::tcp_stream> stream(ioc, *ssl_ctx_);
        beast::get_lowest_layer(stream).connect(results);

        stream.handshake(net::ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::put, "/" + bucket + "/" + key, 11};
        req.set(http::field::host, host);
        req.set(http::field::content_type, content_type);
        req.set(http::field::content_length, std::to_string(data.size()));
        req.set("x-amz-content-sha256", "UNSIGNED-PAYLOAD");  // or compute real
        req.body() = std::string(data.begin(), data.end());
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result() == http::status::ok) {
            return true;
        } else {
            log_replication_event("Remote PUT failed: " + std::to_string(res.result_int()), false);
            return false;
        }
    } catch (const std::exception& e) {
        log_replication_event("Replication push exception: " + std::string(e.what()), false);
        return false;
    }
}

void ReplicationEngine::log_replication_event(const std::string& msg, bool success) {
    std::string level = success ? "INFO" : "ERROR";
    secure_log("[REPLICATION] " + level + ": " + msg);
    // In full impl: append to tamper-evident log with HMAC
}


bool ReplicationEngine::should_attempt_replication(const std::string& endpoint) {
    auto& cb = circuit_breakers_[endpoint];
    auto now = std::chrono::steady_clock::now();

    if (cb.state == CircuitState::Closed) {
        return true;
    }

    if (cb.state == CircuitState::Open) {
        if (now >= cb.next_probe_time) {
            // Move to Half-Open and allow one attempt
            cb.state = CircuitState::HalfOpen;
            cb.failure_count = 0;
            log_replication_event("Circuit Half-Open for " + endpoint + " - probing", true);
            return true;
        }
        log_replication_event("Circuit Open for " + endpoint + " - skipping", false);
        return false;
    }

    // Half-Open: we already decided to probe (should_attempt returns true)
    return true;
}

void ReplicationEngine::record_success(const std::string& endpoint) {
    auto& cb = circuit_breakers_[endpoint];
    if (cb.state != CircuitState::Closed) {
        cb.state = CircuitState::Closed;
        cb.failure_count = 0;
        log_replication_event("Circuit Closed (recovered) for " + endpoint, true);
    }
}

void ReplicationEngine::record_failure(const std::string& endpoint) {
    auto& cb = circuit_breakers_[endpoint];
    auto now = std::chrono::steady_clock::now();

    cb.failure_count++;
    cb.last_failure_time = now;

    if (cb.state == CircuitState::Closed) {
        if (cb.failure_count >= CircuitBreaker::FAILURE_THRESHOLD) {
            cb.state = CircuitState::Open;
            cb.next_probe_time = now + CircuitBreaker::OPEN_TIMEOUT;
            log_replication_event("Circuit Opened for " + endpoint +
                                  " after " + std::to_string(cb.failure_count) +
                                  " failures - will probe again in " +
                                  std::to_string(CircuitBreaker::OPEN_TIMEOUT.count()) + " min", false);
        }
    } else if (cb.state == CircuitState::HalfOpen) {
        // Single failure in half-open → back to open with full timeout
        cb.state = CircuitState::Open;
        cb.next_probe_time = now + CircuitBreaker::OPEN_TIMEOUT;
        cb.failure_count = 0;  // reset for clean count on next cycle
        log_replication_event("Circuit re-opened for " + endpoint +
                              " after probe failure - full timeout", false);
    }
}




ReplicationEngine::ReplicationEngine(StorageManager& storage)
    : storage_(storage) {

    queue_db_path_ = g_config.storage_path + "/replication_queue";
    if (!fs::exists(queue_db_path_)) {
        fs::create_directories(queue_db_path_);
    }

    if (!open_queue_db()) {
        throw std::runtime_error("Failed to open replication queue DB");
    }

    if (!load_next_sequence()) {
        next_sequence_ = 0;
        persist_next_sequence();
    }

    // Optional: recover in-memory cache from DB on startup
    // (for simplicity we scan on first dequeue - or implement full recovery)
}

ReplicationEngine::~ReplicationEngine() {
    Stop();
    if (queue_db_) {
        delete queue_db_;
        queue_db_ = nullptr;
    }
}

bool ReplicationEngine::open_queue_db() {
    leveldb::Options options;
    options.create_if_missing = true;
    options.paranoid_checks = true;      // extra integrity checks
    options.compression = leveldb::kSnappyCompression;

    leveldb::Status status = leveldb::DB::Open(options, queue_db_path_, &queue_db_);
    if (!status.ok()) {
        std::cerr << "LevelDB open failed: " << status.ToString() << std::endl;
        return false;
    }
    return true;
}

bool ReplicationEngine::load_next_sequence() {
    std::string value;
    leveldb::Status s = queue_db_->Get(leveldb::ReadOptions(), "meta:next_seq", &value);
    if (s.ok()) {
        next_sequence_ = std::stoull(value);
        return true;
    }
    return false;
}

void ReplicationEngine::persist_next_sequence() {
    std::string seq_str = std::to_string(next_sequence_);
    queue_db_->Put(leveldb::WriteOptions(), "meta:next_seq", seq_str);
}

bool ReplicationEngine::enqueue_task(const ReplicationTask& task) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    uint64_t seq = next_sequence_++;

    // Serialize task to JSON
    nlohmann::json j;
    j["bucket"]       = task.bucket;
    j["key"]          = task.key;
    j["version_id"]   = task.version_id;
    j["etag"]         = task.etag;
    j["queued_time"]  = std::chrono::system_clock::to_time_t(task.queued_time);

    std::string value = j.dump();

    // Key format: task:0000000000000123 (fixed-width for sorting)
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "task:%016llu", seq);

    leveldb::Status s = queue_db_->Put(leveldb::WriteOptions(), key_buf, value);
    if (!s.ok()) {
        std::cerr << "Failed to enqueue: " << s.ToString() << std::endl;
        return false;
    }

    persist_next_sequence();

    // Also push to in-memory priority queue for fast access
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        task_cache_.push({seq, task});
    }

    return true;
}

bool ReplicationEngine::dequeue_task(ReplicationTask& task) {
    std::lock_guard<std::mutex> cache_lock(cache_mutex_);

    if (task_cache_.empty()) {
        // Optionally scan DB if cache empty (recovery case)
        return false;
    }

    auto qt = task_cache_.top();
    task = qt.task;

    // Remove from LevelDB
    char key_buf[32];
    snprintf(key_buf, sizeof(key_buf), "task:%016llu", qt.sequence);

    leveldb::Status s = queue_db_->Delete(leveldb::WriteOptions(), key_buf);
    if (!s.ok()) {
        std::cerr << "Failed to delete task from DB: " << s.ToString() << std::endl;
        return false;
    }

    task_cache_.pop();
    return true;
}

// QueueReplication now uses persistent enqueue
void ReplicationEngine::QueueReplication(const std::string& bucket, const std::string& key,
                                         const std::string& etag, const std::string& version_id) {
    ReplicationTask task{bucket, key, version_id, etag, std::chrono::system_clock::now()};

    if (enqueue_task(task)) {
        log_replication_event("Persisted replication task to queue: " + bucket + "/" + key, true);
    } else {
        log_replication_event("Failed to persist replication task: " + bucket + "/" + key, false);
    }
}

// Update worker_loop to use dequeue_task
void ReplicationEngine::worker_loop() {
    while (running_) {
        ReplicationTask task;
        bool has_task = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (!task_cache_.empty()) {
                has_task = dequeue_task(task);
            }
        }

        if (!has_task) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // ... rest of worker logic (circuit breaker, replication attempts, etc.) ...
    }
}









