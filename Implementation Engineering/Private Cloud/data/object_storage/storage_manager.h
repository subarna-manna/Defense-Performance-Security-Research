// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/object_storage/storage_manager.h
// Object Storage Manager
// Handles encrypted object persistence on local filesystem

#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <chrono>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include "config_loader.h"  // g_config

using json = nlohmann::json;

struct ObjectMetadata {
    std::string etag;                    // SHA256 hex of plaintext
    size_t size_bytes = 0;
    std::string content_type = "application/octet-stream";
    std::chrono::system_clock::time_point last_modified;
    std::string version_id;              // for future versioning
};

enum class StorageTier {
    Hot,      // RAM + NVMe
    Warm,     // Fast local disk
    Cold,     // Slower local / remote
    Archive   // Offline / deep archive (future)
    // ... existing Hot/Warm/Cold ...
    RemoteCold
};


class StorageManager {
private:
    std::string base_path_;              // g_config.storage_path + "/objects"
    std::mutex mutex_;                   // protect metadata ops

    std::string get_object_path(const std::string& bucket, const std::string& key) const;
    std::string get_meta_path(const std::string& bucket, const std::string& key) const;

    // Crypto helpers
    bool encrypt_data(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext,
                      std::vector<uint8_t>& tag, std::vector<uint8_t>& iv) const;
    bool decrypt_data(const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& tag,
                      const std::vector<uint8_t>& iv, std::vector<uint8_t>& plaintext) const;

    // Key derivation (placeholder; integrate with key_store.c / HSM in production)
    std::vector<uint8_t> derive_object_key(const std::string& bucket, const std::string& key) const;



    // Existing...
    std::map<StorageTier, std::string> tier_paths_;  // tier → base directory

    // New: tier-aware object path
    std::string get_tiered_object_path(const std::string& bucket, const std::string& key,
                                       StorageTier tier) const;

    // New: move object between tiers
    bool move_object_to_tier(const std::string& bucket, const std::string& key,
                             StorageTier from_tier, StorageTier to_tier);

    bool is_remote_cold_enabled() const { return g_config.remote_cold_enabled && !g_config.remote_cold_endpoint.empty(); }
    bool upload_to_remote(const std::string& bucket, const std::string& key, const std::vector<uint8_t>& data,
                          const std::string& content_type, std::string& remote_etag);
    bool fetch_from_remote(const std::string& bucket, const std::string& key, std::vector<uint8_t>& data,
                           ObjectMetadata& meta);
    bool delete_from_remote(const std::string& bucket, const std::string& key);


    // New: Get compression level for a tier
    int get_compression_level(StorageTier tier) const;

    // New: Compress/decompress helpers
    bool compress_plaintext(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& compressed) const;
    bool decompress_plaintext(const std::vector<uint8_t>& compressed, std::vector<uint8_t>& plaintext, int level) const;

    // Override PutObject to compress based on target tier
    bool PutObject(const std::string& bucket, const std::string& key,
                const std::vector<uint8_t>& data, const std::string& content_type = "",
                StorageTier target_tier = StorageTier::Warm);  // default Warm




public:
    StorageManager();

    bool PutObject(const std::string& bucket, const std::string& key,
                   const std::vector<uint8_t>& data, const std::string& content_type = "");

    // bool GetObject(const std::string& bucket, const std::string& key,
    //                std::vector<uint8_t>& data, ObjectMetadata& meta);

    // Override/extend GetObject to fetch from remote if missing locally
    bool GetObject(const std::string& bucket, const std::string& key,
        std::vector<uint8_t>& data, ObjectMetadata& meta, bool force_local = false);

    // New: Demote to remote cold
    bool DemoteToRemoteCold(const std::string& bucket, const std::string& key);

    bool DeleteObject(const std::string& bucket, const std::string& key);

    bool ListObjects(const std::string& bucket, std::vector<std::string>& keys);

    bool ObjectExists(const std::string& bucket, const std::string& key) const;

    // Future: versioning support
    // bool PutObjectVersioned(...);

    // New public API
    StorageTier GetObjectTier(const std::string& bucket, const std::string& key) const;
    bool PromoteObject(const std::string& bucket, const std::string& key);  // → Hot
    bool DemoteObject(const std::string& bucket, const std::string& key);   // → Warm/Cold

};






#endif

// Add these members
private:
    std::string multipart_path_;  // base_path_ + "/multipart"

    std::mutex multipart_mutex_;

    struct MultipartUpload {
        std::string upload_id;
        std::string bucket;
        std::string key;
        std::chrono::system_clock::time_point initiated;
        std::map<int, std::string> parts;  // part_number → part_file_path (encrypted)
        std::string content_type;
    };

    std::map<std::string, MultipartUpload> active_uploads_;  // upload_id → info (in-memory for now)

public:
    // New public methods
    std::string InitiateMultipartUpload(const std::string& bucket, const std::string& key,
                                        const std::string& content_type = "");

    bool UploadPart(const std::string& bucket, const std::string& key,
                    const std::string& upload_id, int part_number,
                    const std::vector<uint8_t>& data);

    bool CompleteMultipartUpload(const std::string& bucket, const std::string& key,
                                 const std::string& upload_id,
                                 const std::vector<std::pair<int, std::string>>& etags);  // part_num, etag

    bool AbortMultipartUpload(const std::string& upload_id);

private:
    std::string get_multipart_dir(const std::string& upload_id) const;
    std::string get_part_path(const std::string& upload_id, int part_number) const;
    std::string get_upload_meta_path(const std::string& upload_id) const;
    bool load_upload_meta(const std::string& upload_id, MultipartUpload& upload);
    void save_upload_meta(const MultipartUpload& upload);