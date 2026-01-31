// common/config_loader.h
#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>
#include <vector>
#include <atomic>

struct CloudConfig {
    // Load Balancer
    int max_backends = 128;
    int healthcheck_interval_ms = 1000;
    std::vector<std::string> backend_ips;  // e.g., "192.168.1.10:8080"
    std::string lb_listen_addr = "0.0.0.0:443";

    // Security / Zero Trust
    std::string ca_cert_path;
    std::string server_cert_path;
    std::string server_key_path;
    bool enforce_mtls = true;
    std::string hsm_endpoint;  // for key_store.c

    // Autoscaling
    double cpu_threshold_high = 80.0;
    double cpu_threshold_low = 30.0;
    int min_instances = 2;
    int max_instances = 20;
    int scale_cooldown_sec = 60;

    // Cache
    size_t cache_max_size_mb = 1024;
    std::string eviction_policy = "LRU";  // LRU or LFU

    // Replication / Storage
    std::vector<std::string> replica_regions;  // e.g., "region1", "region2"
    std::string storage_path = "/data/objects";
    bool enable_versioning = true;

    // Logging / Telemetry
    std::string log_path = "/var/log/defense_cloud.log";
    int log_level = 0;  // 0=debug, 1=info, etc.

    // Network Isolation
    std::string microseg_policy_file;  // JSON policies

    // Security / Zero Trust (already partial)
    std::string ca_cert_path = "/etc/certs/ca.pem";              // Trusted CA for mTLS client validation
    std::string server_cert_path = "/etc/certs/server-chain.pem"; // Full chain (server cert + intermediates)
    std::string server_key_path = "/etc/certs/server-key.pem";   // Server private key (unencrypted or callback if needed)
    bool enforce_mtls = true;                                     // Require client cert + verify
    std::string dh_params_path = "/etc/certs/dh4096.pem";        // Optional for TLS <1.3
    bool backend_tls = false;                                     // Future: enable TLS to backends
    std::string backend_ca_cert_path;                             // For backend cert verification


};

extern CloudConfig g_config;
bool LoadConfig(const std::string& encrypted_config_path, const std::string& decryption_key);

#endif


uint64_t hot_min_access_count = 100;
double hot_threshold_score = 50.0;
int hot_decay_interval_sec = 300;
double hot_decay_factor = 0.9;


std::string storage_hot_path   = "/mnt/nvme/hot";     // fast SSD/NVMe
std::string storage_warm_path  = "/mnt/sata/warm";    // HDD/SSD
std::string storage_cold_path  = "/mnt/slow/cold";    // slower disks
double      hot_promote_threshold = 75.0;
double      hot_demote_threshold  = 25.0;
int         tier_check_interval_min = 15;


// In struct CloudConfig
std::string remote_cold_endpoint = "";              // e.g., "https://cold-cluster.example.com:9000"
std::string remote_cold_access_key = "";
std::string remote_cold_secret_key = "";
std::string remote_cold_bucket = "defense-archive";
int remote_cold_demote_age_days = 90;               // Age threshold for demotion
double remote_cold_heat_threshold = 10.0;           // Only demote if heat < this
bool remote_cold_enabled = false;

// ... existing remote fields ...
int remote_cold_demote_age_days = 90;
double remote_cold_heat_threshold = 10.0;


// In struct CloudConfig
int compression_hot_level = 0;      // 0 = disabled
int compression_warm_level = 5;     // Medium (good balance)
int compression_cold_level = 9;     // High (max savings)
int compression_remote_level = 9;   // High for bandwidth savings

bool enable_deduplication = true;
size_t dedup_avg_chunk_size = 4194304;     // 4 MiB





