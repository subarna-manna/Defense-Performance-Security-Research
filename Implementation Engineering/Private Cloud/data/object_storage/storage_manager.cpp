// Defence Research/Cloud Options/Implementation Engineering/Private Cloud/data/object_storage/storage_manager.cpp

#include "storage_manager.h"
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <iostream>
#include <uuid/uuid.h>  // for generate upload_id (link -luuid)
#include <dirent.h>     // for cleanup on abort
#include <zlib.h>


namespace fs = std::filesystem;

StorageManager::StorageManager() {
    base_path_ = g_config.storage_path;
    if (base_path_.empty()) base_path_ = "/var/defense_cloud/storage/objects";
    
    multipart_path_ = base_path_ + "/multipart";
    if (!fs::exists(multipart_path_)) fs::create_directories(multipart_path_);

    if (!fs::exists(base_path_)) {
        fs::create_directories(base_path_);
    }

    // ... existing init ...

    // Tier paths from config (extend g_config)
    tier_paths_[StorageTier::Hot]   = g_config.storage_hot_path.empty()   ? base_path_ + "/hot"   : g_config.storage_hot_path;
    tier_paths_[StorageTier::Warm]  = g_config.storage_warm_path.empty()  ? base_path_ + "/warm"  : g_config.storage_warm_path;
    tier_paths_[StorageTier::Cold]  = g_config.storage_cold_path.empty()  ? base_path_ + "/cold"  : g_config.storage_cold_path;
    tier_paths_[StorageTier::Archive] = "";  // future: external

    for (const auto& [tier, path] : tier_paths_) {
        if (!path.empty() && !fs::exists(path)) {
            fs::create_directories(path);
        }
    }


}

// Helper: bucket/key → filesystem path (use hash of key to avoid illegal chars)
std::string StorageManager::get_object_path(const std::string& bucket, const std::string& key) const {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(key.c_str()), key.size(), hash);

    std::stringstream ss;
    ss << base_path_ << "/" << bucket << "/";
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

std::string StorageManager::get_meta_path(const std::string& bucket, const std::string& key) const {
    return get_object_path(bucket, key) + ".meta";
}

// Derive per-object key (placeholder – use HKDF or HSM-derived in production)
std::vector<uint8_t> StorageManager::derive_object_key(const std::string& bucket, const std::string& key) const {
    std::string input = bucket + ":" + key + ":defense-salt-2026";  // poor man's salt; replace
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    return {hash, hash + SHA256_DIGEST_LENGTH};
}

bool StorageManager::encrypt_data(const std::vector<uint8_t>& plaintext,
                                  std::vector<uint8_t>& ciphertext,
                                  std::vector<uint8_t>& tag,
                                  std::vector<uint8_t>& iv) const {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    iv.resize(12);
    RAND_bytes(iv.data(), iv.size());

    std::vector<uint8_t> key = derive_object_key("", "");  // TODO: pass bucket/key

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    ciphertext.resize(plaintext.size() + 16);  // room for tag
    int len = 0, total_len = 0;

    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total_len += len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total_len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total_len += len;

    tag.resize(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    ciphertext.resize(total_len);
    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool StorageManager::decrypt_data(const std::vector<uint8_t>& ciphertext,
                                  const std::vector<uint8_t>& tag,
                                  const std::vector<uint8_t>& iv,
                                  std::vector<uint8_t>& plaintext) const {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    std::vector<uint8_t> key = derive_object_key("", "");  // TODO

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    plaintext.resize(ciphertext.size());
    int len = 0, total_len = 0;

    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    total_len += len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<unsigned char*>(tag.data()));

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total_len, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        return false;  // auth fail
    }
    total_len += len;

    plaintext.resize(total_len);
    EVP_CIPHER_CTX_free(ctx);
    return true;
}

bool StorageManager::PutObject(const std::string& bucket, const std::string& key,
                               const std::vector<uint8_t>& data, const std::string& content_type) {
    std::lock_guard<std::mutex> lock(mutex_);

    fs::path obj_path = get_object_path(bucket, key);
    fs::path dir = obj_path.parent_path();
    if (!fs::exists(dir)) fs::create_directories(dir);

    // Encrypt
    std::vector<uint8_t> ciphertext, tag, iv;
    if (!encrypt_data(data, ciphertext, tag, iv)) {
        std::cerr << "Encryption failed for " << bucket << "/" << key << std::endl;
        return false;
    }

    // Write encrypted data
    std::ofstream ofs(obj_path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(iv.data()), iv.size());
    ofs.write(reinterpret_cast<const char*>(tag.data()), tag.size());
    ofs.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    ofs.close();

    // Metadata
    ObjectMetadata meta;
    unsigned char etag_hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), etag_hash);
    std::stringstream etag_ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        etag_ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(etag_hash[i]);
    }
    meta.etag = etag_ss.str();
    meta.size_bytes = data.size();
    meta.content_type = content_type;
    meta.last_modified = std::chrono::system_clock::now();

    json j;
    j["etag"] = meta.etag;
    j["size_bytes"] = meta.size_bytes;
    j["content_type"] = meta.content_type;
    j["last_modified"] = std::chrono::system_clock::to_time_t(meta.last_modified);

    std::ofstream meta_ofs(get_meta_path(bucket, key));
    meta_ofs << j.dump(2);
    meta_ofs.close();

    return true;
}

bool StorageManager::GetObject(const std::string& bucket, const std::string& key,
                               std::vector<uint8_t>& data, ObjectMetadata& meta) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string obj_path = get_object_path(bucket, key);
    if (!fs::exists(obj_path)) return false;

    std::ifstream ifs(obj_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;

    size_t file_size = ifs.tellg();
    if (file_size < 28) return false;  // iv(12) + tag(16) min

    ifs.seekg(0, std::ios::beg);

    std::vector<uint8_t> iv(12), tag(16), ciphertext(file_size - 28);
    ifs.read(reinterpret_cast<char*>(iv.data()), 12);
    ifs.read(reinterpret_cast<char*>(tag.data()), 16);
    ifs.read(reinterpret_cast<char*>(ciphertext.data()), ciphertext.size());
    ifs.close();

    if (!decrypt_data(ciphertext, tag, iv, data)) {
        std::cerr << "Decryption / auth failed for " << bucket << "/" << key << std::endl;
        return false;
    }

    // Load metadata
    std::ifstream meta_ifs(get_meta_path(bucket, key));
    if (meta_ifs) {
        json j = json::parse(meta_ifs);
        meta.etag = j.value("etag", "");
        meta.size_bytes = j.value("size_bytes", 0ULL);
        meta.content_type = j.value("content_type", "application/octet-stream");
        auto ts = j.value("last_modified", 0LL);
        meta.last_modified = std::chrono::system_clock::from_time_t(ts);
    }

    return true;
}

bool StorageManager::DeleteObject(const std::string& bucket, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::remove(get_object_path(bucket, key));
    fs::remove(get_meta_path(bucket, key));
    return true;
}

bool StorageManager::ListObjects(const std::string& bucket, std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string bucket_dir = base_path_ + "/" + bucket;
    if (!fs::exists(bucket_dir)) return false;

    for (const auto& entry : fs::directory_iterator(bucket_dir)) {
        if (entry.is_regular_file() && entry.path().extension() != ".meta") {
            // In real impl: reverse hash to key or store key in meta
            // For now: return filename (hashed)
            keys.push_back(entry.path().filename().string());
        }
    }
    return true;
}

bool StorageManager::ObjectExists(const std::string& bucket, const std::string& key) const {
    return fs::exists(get_object_path(bucket, key));
}





StorageManager::StorageManager() {
    // ... existing init ...

    // Tier paths from config (extend g_config)
    tier_paths_[StorageTier::Hot]   = g_config.storage_hot_path.empty()   ? base_path_ + "/hot"   : g_config.storage_hot_path;
    tier_paths_[StorageTier::Warm]  = g_config.storage_warm_path.empty()  ? base_path_ + "/warm"  : g_config.storage_warm_path;
    tier_paths_[StorageTier::Cold]  = g_config.storage_cold_path.empty()  ? base_path_ + "/cold"  : g_config.storage_cold_path;
    tier_paths_[StorageTier::Archive] = "";  // future: external

    for (const auto& [tier, path] : tier_paths_) {
        if (!path.empty() && !fs::exists(path)) {
            fs::create_directories(path);
        }
    }
}

std::string StorageManager::get_tiered_object_path(const std::string& bucket,
                                                   const std::string& key,
                                                   StorageTier tier) const {
    auto base = tier_paths_.at(tier);
    if (base.empty()) return "";  // archive tier not local
    return base + "/" + bucket + "/" + compute_hash(key);  // reuse your hash function
}

StorageTier StorageManager::GetObjectTier(const std::string& bucket,
                                          const std::string& key) const {
    for (auto tier : {StorageTier::Hot, StorageTier::Warm, StorageTier::Cold}) {
        if (fs::exists(get_tiered_object_path(bucket, key, tier))) {
            return tier;
        }
    }
    return StorageTier::Cold;  // default
}

bool StorageManager::move_object_to_tier(const std::string& bucket,
                                         const std::string& key,
                                         StorageTier from_tier,
                                         StorageTier to_tier) {
    std::string from_path = get_tiered_object_path(bucket, key, from_tier);
    std::string to_path   = get_tiered_object_path(bucket, key, to_tier);
    std::string meta_from = from_path + ".meta";
    std::string meta_to   = to_path + ".meta";

    if (!fs::exists(from_path)) return false;

    fs::create_directories(fs::path(to_path).parent_path());

    std::error_code ec;
    fs::rename(from_path, to_path, ec);
    if (ec) return false;

    if (fs::exists(meta_from)) {
        fs::rename(meta_from, meta_to, ec);
        if (ec) {
            // Rollback on meta failure
            fs::rename(to_path, from_path);
            return false;
        }
    }

    secure_log("Moved object " + bucket + "/" + key + " from " +
               std::to_string(static_cast<int>(from_tier)) + " to " +
               std::to_string(static_cast<int>(to_tier)));

    return true;
}

bool StorageManager::PromoteObject(const std::string& bucket, const std::string& key) {
    StorageTier current = GetObjectTier(bucket, key);
    if (current == StorageTier::Hot) return true;
    return move_object_to_tier(bucket, key, current, StorageTier::Hot);
}

bool StorageManager::DemoteObject(const std::string& bucket, const std::string& key) {
    StorageTier current = GetObjectTier(bucket, key);
    if (current == StorageTier::Cold) return true;
    StorageTier target = (current == StorageTier::Hot) ? StorageTier::Warm : StorageTier::Cold;
    return move_object_to_tier(bucket, key, current, target);
}




std::string StorageManager::InitiateMultipartUpload(const std::string& bucket,
        const std::string& key,
        const std::string& content_type) {
    std::lock_guard<std::mutex> lock(multipart_mutex_);

    uuid_t uuid;
    uuid_generate(uuid);
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);

    std::string upload_id = uuid_str;

    MultipartUpload upload;
    upload.upload_id = upload_id;
    upload.bucket = bucket;
    upload.key = key;
    upload.initiated = std::chrono::system_clock::now();
    upload.content_type = content_type.empty() ? "application/octet-stream" : content_type;

    active_uploads_[upload_id] = upload;
    save_upload_meta(upload);

    fs::create_directory(get_multipart_dir(upload_id));

    return upload_id;
}

bool StorageManager::UploadPart(const std::string& bucket, const std::string& key,
    const std::string& upload_id, int part_number,
    const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(multipart_mutex_);

    auto it = active_uploads_.find(upload_id);
    if (it == active_uploads_.end() || it->second.bucket != bucket || it->second.key != key) {
    return false;
    }

    if (part_number < 1 || part_number > 10000) return false;  // S3 limit

    std::string part_path = get_part_path(upload_id, part_number);

    // Encrypt part (same as single object)
    std::vector<uint8_t> ciphertext, tag, iv;
    if (!encrypt_data(data, ciphertext, tag, iv)) return false;

    std::ofstream ofs(part_path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(iv.data()), iv.size());
    ofs.write(reinterpret_cast<const char*>(tag.data()), tag.size());
    ofs.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());

    // Compute ETag = hex(SHA256 of plaintext part)
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    it->second.parts[part_number] = ss.str();

    save_upload_meta(it->second);
    return true;
}

bool StorageManager::CompleteMultipartUpload(const std::string& bucket, const std::string& key,
    const std::string& upload_id,
    const std::vector<std::pair<int, std::string>>& provided_etags) {
    std::lock_guard<std::mutex> lock(multipart_mutex_);

    auto it = active_uploads_.find(upload_id);
    if (it == active_uploads_.end() || it->second.bucket != bucket || it->second.key != key) {
    return false;
    }

    MultipartUpload& upload = it->second;

    // Validate provided ETags match stored
    if (provided_etags.size() != upload.parts.size()) return false;
    for (const auto& [num, etag] : provided_etags) {
    auto pit = upload.parts.find(num);
    if (pit == upload.parts.end() || pit->second != etag) return false;
    }

    // Assemble parts: decrypt each → concatenate plaintext → encrypt as final object
    std::vector<uint8_t> full_plaintext;
    for (int i = 1; i <= 10000; ++i) {  // assume sequential
    auto pit = upload.parts.find(i);
    if (pit == upload.parts.end()) break;

    std::string part_path = get_part_path(upload_id, i);
    std::ifstream ifs(part_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;

    size_t sz = ifs.tellg();
    if (sz < 28) return false;

    ifs.seekg(0);
    std::vector<uint8_t> iv(12), tag(16), ct(sz - 28);
    ifs.read(reinterpret_cast<char*>(iv.data()), 12);
    ifs.read(reinterpret_cast<char*>(tag.data()), 16);
    ifs.read(reinterpret_cast<char*>(ct.data()), ct.size());

    std::vector<uint8_t> part_plain;
    if (!decrypt_data(ct, tag, iv, part_plain)) return false;

    full_plaintext.insert(full_plaintext.end(), part_plain.begin(), part_plain.end());
    }

    // Store as normal object
    bool ok = PutObject(bucket, key, full_plaintext, upload.content_type);
    
    hot_index_->RecordAccess(bucket, key);

    // Cleanup
    fs::remove_all(get_multipart_dir(upload_id));
    fs::remove(get_upload_meta_path(upload_id));
    active_uploads_.erase(it);

    return ok;
}

bool StorageManager::AbortMultipartUpload(const std::string& upload_id) {
    std::lock_guard<std::mutex> lock(multipart_mutex_);

    auto it = active_uploads_.find(upload_id);
    if (it == active_uploads_.end()) return false;

    fs::remove_all(get_multipart_dir(upload_id));
    fs::remove(get_upload_meta_path(upload_id));
    active_uploads_.erase(it);
    return true;
}

// Helpers (implement similarly to get_object_path)
std::string StorageManager::get_multipart_dir(const std::string& upload_id) const {
    return multipart_path_ + "/" + upload_id;
    }

    std::string StorageManager::get_part_path(const std::string& upload_id, int part_number) const {
    return get_multipart_dir(upload_id) + "/part-" + std::to_string(part_number);
    }

    std::string StorageManager::get_upload_meta_path(const std::string& upload_id) const {
    return multipart_path_ + "/" + upload_id + ".json";
}

// Simple JSON save/load for upload meta (expand as needed)
void StorageManager::save_upload_meta(const MultipartUpload& upload) {
    json j;
    j["bucket"] = upload.bucket;
    j["key"] = upload.key;
    j["initiated"] = std::chrono::system_clock::to_time_t(upload.initiated);
    j["content_type"] = upload.content_type;

    json parts_json = json::object();
    for (const auto& [num, etag] : upload.parts) {
    parts_json[std::to_string(num)] = etag;
    }
    j["parts"] = parts_json;

    std::ofstream ofs(get_upload_meta_path(upload.upload_id));
    ofs << j.dump(2);
}

bool StorageManager::load_upload_meta(const std::string& upload_id, MultipartUpload& upload) {
    std::ifstream ifs(get_upload_meta_path(upload_id));
    if (!ifs) return false;

    json j = json::parse(ifs);
    upload.upload_id = upload_id;
    upload.bucket = j["bucket"];
    upload.key = j["key"];
    upload.initiated = std::chrono::system_clock::from_time_t(j["initiated"]);
    upload.content_type = j.value("content_type", "application/octet-stream");

    for (auto& [num_str, etag] : j["parts"].items()) {
    upload.parts[std::stoi(num_str)] = etag.get<std::string>();
    }
    return true;
}



// || modify to add in existing method ||

// In GetObject - check remote if not local
bool StorageManager::GetObject(const std::string& bucket, const std::string& key,
        std::vector<uint8_t>& data, ObjectMetadata& meta, bool force_local) {
    std::string local_path = get_tiered_object_path(bucket, key, GetObjectTier(bucket, key));
    if (fs::exists(local_path)) {
    // Existing local read/decrypt logic...
    // ...
    return true;
    }

    if (!force_local && is_remote_cold_enabled()) {
    if (fetch_from_remote(bucket, key, data, meta)) {
    // After fetch: store locally in Warm (or promote if hot)
    extern HotDataIndex* hot_index_;
    StorageTier target_tier = hot_index_ && hot_index_->IsHot(bucket, key) ? StorageTier::Hot : StorageTier::Warm;
    std::string target_path = get_tiered_object_path(bucket, key, target_tier);
    // Encrypt & write locally (reuse encrypt_data)
    std::vector<uint8_t> ciphertext, tag, iv;
    if (!encrypt_data(data, ciphertext, tag, iv)) return false;
    // Write to disk (similar to PutObject)
    // ...
    // Update meta with local copy
    PutObject(bucket, key, data, meta.content_type);  // Re-put locally
    secure_log("Fetched from remote cold and promoted locally: " + bucket + "/" + key);
    return true;
    }
    }

    return false;
}

// Remote PUT (S3-compatible with SigV4)
bool StorageManager::upload_to_remote(const std::string& bucket, const std::string& key,
            const std::vector<uint8_t>& data, const std::string& content_type,
            std::string& remote_etag) {

        // Compress before remote PUT (extra savings on bandwidth)
        std::vector<uint8_t> compressed;
        if (get_compression_level(StorageTier::RemoteCold) > 0 &&
            compress_plaintext(data, compressed)) {
            // Send compressed; add Content-Encoding: deflate header
            // In Beast request: req.set("Content-Encoding", "deflate");
            // Use compressed as body
        } else {
            // Use original data
        }




    if (!is_remote_cold_enabled()) return false;

    // Use Beast HTTP client (reuse from sync_engine or S3 server logic)
    // Parse endpoint (host/port from g_config.remote_cold_endpoint)
    // Similar to push_object_via_http in sync_engine
    // Add SigV4 signing using access/secret keys (from earlier verify_sigv4 but client-side)

    // Client-side SigV4 (simplified - expand from earlier server code)
    // ... implement signing: canonical request, string-to-sign, signing key derivation ...
    // Send PUT request with Authorization header

    // On success, parse ETag from response
    remote_etag = "remote-etag-placeholder";  // From res["ETag"]
    return true;
}

// Remote GET
bool StorageManager::fetch_from_remote(const std::string& bucket, const std::string& key,
    std::vector<uint8_t>& data, ObjectMetadata& meta) {

    // After receiving body
    if (res["Content-Encoding"] == "deflate") {
        // Decompress
        std::vector<uint8_t> decompressed;
        if (!decompress_plaintext(std::vector<uint8_t>(res.body().begin(), res.body().end()),
                                decompressed, g_config.compression_remote_level)) {
            // Handle failure
        }
        data = std::move(decompressed);
    } else {
        data.assign(res.body().begin(), res.body().end());
    }

    // Similar Beast GET request with SigV4
    // Read body → decrypt → fill data/meta
    // ...
    return true;
}

// Remote DELETE
bool StorageManager::delete_from_remote(const std::string& bucket, const std::string& key) {
    // DELETE request with SigV4
    return true;
}

// Demote to remote
bool StorageManager::DemoteToRemoteCold(const std::string& bucket, const std::string& key) {
    StorageTier current = GetObjectTier(bucket, key);
    if (current == StorageTier::RemoteCold) return true;

    std::vector<uint8_t> data;
    ObjectMetadata meta;
    if (!GetObject(bucket, key, data, meta, true)) return false;  // force local read

    std::string remote_etag;
    if (!upload_to_remote(bucket, key, data, meta.content_type, remote_etag)) return false;

    // Delete local copy
    DeleteObject(bucket, key);

    // Update meta: mark as remote
    meta.remote_etag = remote_etag;
    // Save meta with remote flag
    // ...

    secure_log("Demoted to remote cold: " + bucket + "/" + key);
    return true;
}



// In constructor or init
// Already have tier_paths_; add compression levels from config

int StorageManager::get_compression_level(StorageTier tier) const {
    switch (tier) {
        case StorageTier::Hot:         return g_config.compression_hot_level;
        case StorageTier::Warm:        return g_config.compression_warm_level;
        case StorageTier::Cold:        return g_config.compression_cold_level;
        case StorageTier::RemoteCold:  return g_config.compression_remote_level;
        default:                       return 0;
    }
}

bool StorageManager::compress_plaintext(const std::vector<uint8_t>& plaintext,
                                        std::vector<uint8_t>& compressed) const {
    if (plaintext.empty()) return false;

    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    int level = Z_DEFAULT_COMPRESSION;  // will override per call
    if (deflateInit(&strm, level) != Z_OK) return false;

    strm.avail_in = plaintext.size();
    strm.next_in = const_cast<Bytef*>(plaintext.data());

    compressed.resize(compressBound(plaintext.size()));
    strm.avail_out = compressed.size();
    strm.next_out = compressed.data();

    int ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) return false;

    compressed.resize(compressed.size() - strm.avail_out);
    return true;
}

bool StorageManager::decompress_plaintext(const std::vector<uint8_t>& compressed,
                                          std::vector<uint8_t>& plaintext,
                                          int level) const {
    if (compressed.empty()) return false;

    z_stream strm{};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    if (inflateInit(&strm) != Z_OK) return false;

    strm.avail_in = compressed.size();
    strm.next_in = const_cast<Bytef*>(compressed.data());

    plaintext.resize(compressed.size() * 4);  // Initial guess; resize dynamically
    size_t total_out = 0;

    int ret;
    do {
        strm.avail_out = plaintext.size() - total_out;
        strm.next_out = plaintext.data() + total_out;

        ret = inflate(&strm, Z_NO_FLUSH);

        if (ret == Z_BUF_ERROR) {
            // Need more output space
            size_t new_size = plaintext.size() * 2;
            plaintext.resize(new_size);
            continue;
        }
    } while (ret == Z_OK);

    inflateEnd(&strm);

    if (ret != Z_STREAM_END) return false;

    plaintext.resize(total_out + strm.total_out);
    return true;
}

// Updated PutObject (tier-aware)
bool StorageManager::PutObject(const std::string& bucket, const std::string& key,
                               const std::vector<uint8_t>& data,
                               const std::string& content_type,
                               StorageTier target_tier) {
    std::lock_guard<std::mutex> lock(mutex_);

    int comp_level = get_compression_level(target_tier);
    std::vector<uint8_t> to_encrypt = data;
    bool compressed = false;

    if (comp_level > 0) {
        std::vector<uint8_t> comp_data;
        if (compress_plaintext(data, comp_data)) {
            to_encrypt = std::move(comp_data);
            compressed = true;
        } else {
            // Fallback: uncompressed
            secure_log("Compression failed for " + bucket + "/" + key + " - using uncompressed");
        }
    }

    // Encrypt (as before)
    std::vector<uint8_t> ciphertext, tag, iv;
    if (!encrypt_data(to_encrypt, ciphertext, tag, iv)) return false;

    std::string obj_path = get_tiered_object_path(bucket, key, target_tier);
    fs::path dir = fs::path(obj_path).parent_path();
    if (!fs::exists(dir)) fs::create_directories(dir);

    std::ofstream ofs(obj_path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(iv.data()), iv.size());
    ofs.write(reinterpret_cast<const char*>(tag.data()), tag.size());
    ofs.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    ofs.close();

    // Metadata with compression flag
    ObjectMetadata meta;
    // ... compute etag from original data ...
    meta.size_bytes = data.size();  // Original uncompressed size
    meta.content_type = content_type;
    meta.last_modified = std::chrono::system_clock::now();

    json j;
    j["etag"] = meta.etag;
    j["size_bytes"] = meta.size_bytes;
    j["content_type"] = meta.content_type;
    j["last_modified"] = std::chrono::system_clock::to_time_t(meta.last_modified);
    j["compression_level"] = compressed ? comp_level : 0;

    std::ofstream meta_ofs(get_meta_path(bucket, key));
    meta_ofs << j.dump(2);
    meta_ofs.close();

    return true;
}

// Updated GetObject - decompress if needed
bool StorageManager::GetObject(const std::string& bucket, const std::string& key,
                               std::vector<uint8_t>& data, ObjectMetadata& meta) {
    // ... existing read ciphertext, decrypt to decrypted_data ...

    // After decryption (to decrypted_data)
    int comp_level = 0;
    // Load from meta
    std::ifstream meta_ifs(get_meta_path(bucket, key));
    if (meta_ifs) {
        json j = json::parse(meta_ifs);
        // ... load other fields ...
        comp_level = j.value("compression_level", 0);
    }

    if (comp_level > 0) {
        if (!decompress_plaintext(decrypted_data, data, comp_level)) {
            secure_log("Decompression failed for " + bucket + "/" + key);
            return false;
        }
    } else {
        data = std::move(decrypted_data);
    }

    return true;
}




bool StorageManager::PutObject(const std::string& bucket, const std::string& key,
    const std::vector<uint8_t>& data, ...) {
// ... tier decision ...

if (!g_config.enable_deduplication) {
// fallback to previous full-object logic
return legacy_put(...);
}

// Split into chunks (Rabin CDC - simple fixed-size for v1)
const size_t chunk_size = 4 * 1024 * 1024; // 4 MiB
nlohmann::json manifest;
manifest["version"] = 1;
manifest["chunks"] = nlohmann::json::array();
manifest["original_size"] = data.size();

size_t offset = 0;
for (size_t i = 0; i < data.size(); i += chunk_size) {
size_t len = std::min(chunk_size, data.size() - i);
std::vector<uint8_t> chunk(data.begin() + i, data.begin() + i + len);

bool was_new;
std::string chunk_hash = dedup_store_->StoreChunk(chunk, was_new);

manifest["chunks"].push_back({
{"hash", chunk_hash},
{"offset", offset},
{"length", len}
});

offset += len;
}

// Store manifest (small!) as the actual object content
std::string manifest_str = manifest.dump();
std::vector<uint8_t> manifest_data(manifest_str.begin(), manifest_str.end());

// Proceed with encryption & tier storage of manifest
return legacy_put_with_data(bucket, key, manifest_data, content_type, target_tier);
}




bool StorageManager::GetObject(...) {
    // Read stored object (manifest)
    std::vector<uint8_t> stored_data;
    if (!legacy_get(bucket, key, stored_data, meta)) return false;

    // Parse manifest
    std::string manifest_str(stored_data.begin(), stored_data.end());
    auto manifest = nlohmann::json::parse(manifest_str);

    std::vector<uint8_t> full_data;
    full_data.reserve(manifest["original_size"].get<size_t>());

    for (const auto& chunk_info : manifest["chunks"]) {
        std::string hash = chunk_info["hash"];
        std::vector<uint8_t> chunk;
        if (!dedup_store_->GetChunk(hash, chunk)) return false;

        full_data.insert(full_data.end(), chunk.begin(), chunk.end());
    }

    data = std::move(full_data);
    return true;
}





void DedupChunkStore::gc_unused_chunks() {
    // Iterate LevelDB keys
    leveldb::Iterator* it = db_->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        std::string hash = it->key().ToString();
        uint64_t ref = std::stoull(it->value().ToString());
        if (ref == 0) {
            std::string path = get_chunk_path(hash);
            fs::remove(path);
            db_->Delete(leveldb::WriteOptions(), hash);
        }
    }
    delete it;
}












// || ------- Integration with StorageManager ------- || 
// In storage_manager.cpp, after successful PutObject / CompleteMultipartUpload:
// C++// Assume global or injected ReplicationEngine* repl_engine_;
// if (repl_engine_) {
//     repl_engine_->QueueReplication(bucket, key, meta.etag);
// }



