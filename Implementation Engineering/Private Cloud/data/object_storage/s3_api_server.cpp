// data/object_storage/s3_api_server.cpp

#include "s3_api_server.h"
#include <boost/asio/strand.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <map>
#include <cctype>
#include <cstring>  // for std::memcmp



void S3ApiSession::run() {
    do_read();
}

void S3ApiSession::do_read() {
    req_ = {};
    stream_.expires_after(std::chrono::seconds(30));
    http::async_read(stream_, buffer_, req_,
        beast::bind_front_handler(&S3ApiSession::on_read, shared_from_this()));
}

void S3ApiSession::on_read(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) {
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
        return;
    }
    if (ec) return;

    handle_request();
}


bool verify_sigv4(const http::request<http::string_body>& req) {
    std::string auth = req["Authorization"].to_string();
    if (auth.empty() || auth.find("AWS4-HMAC-SHA256") != 0) {
        return false;
    }

    // Parse Authorization header
    std::string credential, signed_headers_str, signature;
    size_t cred_pos = auth.find("Credential=");
    if (cred_pos == std::string::npos) return false;
    size_t comma1 = auth.find(',', cred_pos);
    credential = auth.substr(cred_pos + 11, comma1 - cred_pos - 11);

    size_t signed_pos = auth.find("SignedHeaders=");
    if (signed_pos == std::string::npos) return false;
    size_t comma2 = auth.find(',', signed_pos);
    signed_headers_str = auth.substr(signed_pos + 14, comma2 - signed_pos - 14);

    size_t sig_pos = auth.find("Signature=");
    if (sig_pos == std::string::npos) return false;
    signature = auth.substr(sig_pos + 10);

    // Parse credential parts: AKID/date/region/service/aws4_request
    std::vector<std::string> cred_parts;
    std::stringstream cred_ss(credential);
    std::string part;
    while (std::getline(cred_ss, part, '/')) cred_parts.push_back(part);

    if (cred_parts.size() != 5 || cred_parts[4] != "aws4_request") return false;
    std::string access_key = cred_parts[0];
    std::string date = cred_parts[1];           // yyyymmdd
    std::string region = cred_parts[2];
    std::string service = cred_parts[3];        // should be "s3"

    // Find secret key
    auto it = credentials.find(access_key);
    if (it == credentials.end()) return false;
    std::string secret_key = it->second;

    // Get x-amz-date (must match date in credential scope)
    std::string x_amz_date = req["x-amz-date"].to_string();
    if (x_amz_date.empty() || x_amz_date.substr(0, 8) != date) return false;

    // Compute payload hash (x-amz-content-sha256 or SHA256(body))
    std::string payload_hash = req["x-amz-content-sha256"].to_string();
    if (payload_hash.empty()) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(req.body().data()), req.body().size(), hash);
        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        payload_hash = ss.str();
    }

    // Build Canonical Headers & Signed Headers
    std::map<std::string, std::string> canonical_headers;
    std::vector<std::string> signed_headers_vec;
    {
        std::stringstream sh_ss(signed_headers_str);
        std::string h;
        while (std::getline(sh_ss, h, ';')) {
            std::transform(h.begin(), h.end(), h.begin(), ::tolower);
            signed_headers_vec.push_back(h);
        }
        std::sort(signed_headers_vec.begin(), signed_headers_vec.end());
    }

    std::string canonical_headers_str;
    for (const auto& h : signed_headers_vec) {
        std::string val = req[boost::beast::http::field::unknown(h)].to_string();  // case-insensitive?
        // Trim & lowercase header name
        std::string lower_h = h;
        std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);
        canonical_headers_str += lower_h + ":" + val + "\n";  // trim val in production
    }

    std::string signed_headers = signed_headers_str;  // already ; separated, but we use it as-is

    // Canonical URI (decode & normalize)
    std::string canonical_uri = req.target().to_string();
    size_t query_pos = canonical_uri.find('?');
    if (query_pos != std::string::npos) canonical_uri = canonical_uri.substr(0, query_pos);
    // Percent-decode if needed (simplified; add urldecode in production)

    // Canonical Query String (sorted, encoded - simplified, assuming no query for now)
    std::string canonical_query = "";  // parse & sort if present

    // Canonical Request
    std::ostringstream canon_req;
    canon_req << req.method_string() << "\n"
              << canonical_uri << "\n"
              << canonical_query << "\n"
              << canonical_headers_str << "\n"
              << signed_headers << "\n"
              << payload_hash;

    std::string canonical_request = canon_req.str();

    // Hash of canonical request
    unsigned char canon_hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(canonical_request.data()), canonical_request.size(), canon_hash);
    std::string canon_hash_hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        canon_hash_hex += "0123456789abcdef"[canon_hash[i] >> 4] +
                          "0123456789abcdef"[canon_hash[i] & 0x0F];

    // String to Sign
    std::string scope = date + "/" + region + "/" + service + "/aws4_request";
    std::string string_to_sign = "AWS4-HMAC-SHA256\n" +
                                 x_amz_date + "\n" +
                                 scope + "\n" +
                                 canon_hash_hex;

    // Derive Signing Key
    auto hmac_sha256 = [](const unsigned char* key, size_t key_len,
                          const unsigned char* data, size_t data_len,
                          unsigned char* out) {
        unsigned int len = SHA256_DIGEST_LENGTH;
        HMAC(EVP_sha256(), key, key_len, data, data_len, out, &len);
    };

    unsigned char k_date[SHA256_DIGEST_LENGTH];
    std::string date_key_str = "AWS4" + secret_key;
    hmac_sha256(reinterpret_cast<const unsigned char*>(date_key_str.data()), date_key_str.size(),
                reinterpret_cast<const unsigned char*>(date.data()), date.size(), k_date);

    unsigned char k_region[SHA256_DIGEST_LENGTH];
    hmac_sha256(k_date, SHA256_DIGEST_LENGTH,
                reinterpret_cast<const unsigned char*>(region.data()), region.size(), k_region);

    unsigned char k_service[SHA256_DIGEST_LENGTH];
    hmac_sha256(k_region, SHA256_DIGEST_LENGTH,
                reinterpret_cast<const unsigned char*>(service.data()), service.size(), k_service);

    unsigned char signing_key[SHA256_DIGEST_LENGTH];
    hmac_sha256(k_service, SHA256_DIGEST_LENGTH,
                reinterpret_cast<const unsigned char*>("aws4_request"), 12, signing_key);

    // Compute expected signature
    unsigned char computed_sig[SHA256_DIGEST_LENGTH];
    hmac_sha256(signing_key, SHA256_DIGEST_LENGTH,
                reinterpret_cast<const unsigned char*>(string_to_sign.data()), string_to_sign.size(), computed_sig);

    std::string computed_sig_hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        computed_sig_hex += "0123456789abcdef"[computed_sig[i] >> 4] +
                            "0123456789abcdef"[computed_sig[i] & 0x0F];

    // Constant-time compare
    return computed_sig_hex == signature;
}



void S3ApiSession::handle_request() {
    auto target = std::string(req_.target());
    if (target.empty() || target[0] != '/') {
        send_error(http::status::bad_request, "Invalid path");
        return;
    }

    if (!verify_sigv4(req_)) {
        send_error(http::status::unauthorized, "SignatureDoesNotMatch");
        return;
    }

    // Parse /bucket/key or /bucket?query
    size_t slash_pos = target.find('/', 1);
    std::string bucket = (slash_pos != std::string::npos)
        ? target.substr(1, slash_pos - 1)
        : target.substr(1);

    std::string key_or_query = (slash_pos != std::string::npos)
        ? target.substr(slash_pos + 1)
        : "";



    // Detect multipart operations
    bool is_multipart_init   = (req_.method() == http::verb::post && key_or_query == "uploads");
    bool is_upload_part      = (req_.method() == http::verb::put  && key_or_query.find("partNumber=") != std::string::npos && key_or_query.find("uploadId=") != std::string::npos);
    bool is_complete_multipart = (req_.method() == http::verb::post && key_or_query.find("uploadId=") != std::string::npos);
    bool is_abort_multipart  = (req_.method() == http::verb::delete_ && key_or_query.find("uploadId=") != std::string::npos);

    if (is_multipart_init) {
        std::string upload_id = storage_.InitiateMultipartUpload(bucket, key, req_["Content-Type"].to_string());
        http::response<http::string_body> res{http::status::ok, req_.version()};
        res.set(http::field::content_type, "application/xml");
        res.body() = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<InitiateMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                     "  <Bucket>" + bucket + "</Bucket>\n"
                     "  <Key>" + key + "</Key>\n"
                     "  <UploadId>" + upload_id + "</UploadId>\n"
                     "</InitiateMultipartUploadResult>";
        res.prepare_payload();
        send_response(std::move(res));
        return;
    }

    if (is_upload_part) {
        // Parse query: partNumber=5&uploadId=abc...
        int part_num = 0;
        std::string upload_id;
        // Naive parse - improve with proper parser
        size_t pn_pos = key_or_query.find("partNumber=");
        if (pn_pos != std::string::npos) part_num = std::stoi(key_or_query.substr(pn_pos + 11));
        size_t uid_pos = key_or_query.find("uploadId=");
        if (uid_pos != std::string::npos) upload_id = key_or_query.substr(uid_pos + 9);

        std::vector<uint8_t> data(req_.body().size());
        std::copy(req_.body().begin(), req_.body().end(), data.begin());

        if (storage_.UploadPart(bucket, key, upload_id, part_num, data)) {
            http::response<http::empty_body> res{http::status::ok, req_.version()};
            res.set(http::field::etag, "\"part-etag-placeholder\"");  // Use real ETag
            send_response(std::move(res));
        } else {
            send_error(http::status::bad_request, "InvalidPart");
        }
        return;
    }

    if (is_complete_multipart || is_abort_multipart) {
        // Parse uploadId from query
        std::string upload_id;
        size_t uid_pos = key_or_query.find("uploadId=");
        if (uid_pos != std::string::npos) upload_id = key_or_query.substr(uid_pos + 9);

        if (is_complete_multipart) {
            // Body is XML: <CompleteMultipartUpload><Part><PartNumber>1</PartNumber><ETag>abc</ETag></Part>...</CompleteMultipartUpload>
            // Parse XML (simplified - in production use tinyxml2 or pugixml)
            // For demo: assume we parse provided_etags
            std::vector<std::pair<int, std::string>> etags;  // fill from body parse

            if (storage_.CompleteMultipartUpload(bucket, key, upload_id, etags)) {
                http::response<http::string_body> res{http::status::ok, req_.version()};
                res.set(http::field::content_type, "application/xml");
                res.body() = "<CompleteMultipartUploadResult>...</CompleteMultipartUploadResult>";
                send_response(std::move(res));
            } else {
                send_error(http::status::bad_request, "InvalidPartOrder or ETag mismatch");
            }
        } else {  // abort
            if (storage_.AbortMultipartUpload(upload_id)) {
                http::response<http::empty_body> res{http::status::no_content, req_.version()};
                send_response(std::move(res));
            } else {
                send_error(http::status::not_found, "NoSuchUpload");
            }
        }
        return;
    }


    // || usage ||

    // aws --endpoint-url http://localhost:9000 s3 cp largefile.bin s3://mybucket/largefile.bin --no-verify-ssl








    // Existing single-object handling...









    bool is_list = (req_.method() == http::verb::get &&
                    key_or_query.find("?list-type=2") != std::string::npos);

    if (is_list) {
        handle_list_objects_v2(bucket);
        return;
    }

    std::string key = key_or_query;

    switch (req_.method()) {
        case http::verb::put:
            handle_put_object(bucket, key);
            break;
        case http::verb::get:
            handle_get_object(bucket, key);
            break;
        case http::verb::delete_:
            handle_delete_object(bucket, key);
            break;
        default:
            send_error(http::status::method_not_allowed, "Method not allowed");
    }
}

void S3ApiSession::handle_put_object(const std::string& bucket, const std::string& key) {
    if (key.empty()) {
        send_error(http::status::bad_request, "Key required");
        return;
    }

    std::vector<uint8_t> data(req_.body().size());
    std::copy(req_.body().begin(), req_.body().end(), data.begin());

    std::string content_type = req_["Content-Type"].to_string();
    if (content_type.empty()) content_type = "application/octet-stream";

    if (storage_.PutObject(bucket, key, data, content_type)) {
        http::response<http::empty_body> res{http::status::ok, req_.version()};
        res.set(http::field::content_length, "0");
        res.set(http::field::etag, "\"fake-etag-for-now\"");  // Use real ETag from meta
        send_response(std::move(res));
    } else {
        send_error(http::status::internal_server_error, "Put failed");
    }

    // After successful Get/Put
    extern HotDataIndex* hot_index_;
    hot_index_->RecordAccess(bucket, key);

    // In Get: after reading from disk, if hot → promote
    if (hot_index_->GetHeatScore(bucket, key) > hot_threshold_score_) {
        storage_.PromoteObject(bucket, key);
    }


}

void S3ApiSession::handle_get_object(const std::string& bucket, const std::string& key) {
    if (key.empty()) {
        send_error(http::status::bad_request, "Key required");
        return;
    }

    std::vector<uint8_t> data;
    StorageManager::ObjectMetadata meta;
    if (storage_.GetObject(bucket, key, data, meta)) {
        http::response<http::string_body> res{http::status::ok, req_.version()};
        res.set(http::field::content_type, meta.content_type);
        res.set(http::field::content_length, std::to_string(meta.size_bytes));
        res.set(http::field::last_modified, "");  // format properly in production
        res.set(http::field::etag, "\"" + meta.etag + "\"");
        res.body() = std::string(data.begin(), data.end());
        res.prepare_payload();
        send_response(std::move(res));
    } else {
        send_error(http::status::not_found, "NoSuchKey");
    }


        // After successful Get/Put
    extern HotDataIndex* hot_index_;
    hot_index_->RecordAccess(bucket, key);

    // In Get: after reading from disk, if hot → promote
    if (hot_index_->GetHeatScore(bucket, key) > hot_threshold_score_) {
        storage_.PromoteObject(bucket, key);
    }


    // After fetch success
    if (hot_index_->GetHeatScore(bucket, key) > g_config.hot_threshold_score) {
        storage_.PromoteObject(bucket, key);  // Local promotion after remote fetch
    }



}

void S3ApiSession::handle_delete_object(const std::string& bucket, const std::string& key) {
    if (key.empty()) {
        send_error(http::status::bad_request, "Key required");
        return;
    }

    if (storage_.DeleteObject(bucket, key)) {
        http::response<http::empty_body> res{http::status::no_content, req_.version()};
        send_response(std::move(res));
    } else {
        send_error(http::status::not_found, "NoSuchKey");
    }
}

void S3ApiSession::handle_list_objects_v2(const std::string& bucket) {
    auto params = http::url_query(req_.target());  // Beast has no built-in parser; simple impl below

    std::string prefix = "";
    size_t max_keys = 1000;
    std::string continuation_token = "";

    // Very basic query parsing (expand with proper parser in production)
    auto query_start = std::string(req_.target()).find('?');
    if (query_start != std::string::npos) {
        std::string query = std::string(req_.target()).substr(query_start + 1);
        // Parse key=value&... (very naive)
        // ...
    }

    std::vector<std::string> all_keys;
    if (!storage_.ListObjects(bucket, all_keys)) {
        send_error(http::status::not_found, "NoSuchBucket");
        return;
    }

    // Filter prefix, apply continuation, max-keys (simplified)
    std::vector<std::string> result_keys;
    for (const auto& k : all_keys) {
        if (prefix.empty() || k.find(prefix) == 0) {
            result_keys.push_back(k);
        }
    }
    // Apply max_keys, continuation...

    std::string xml = generate_list_xml(result_keys, prefix, continuation_token, max_keys);

    http::response<http::string_body> res{http::status::ok, req_.version()};
    res.set(http::field::content_type, "application/xml");
    res.body() = xml;
    res.prepare_payload();
    send_response(std::move(res));
}

std::string S3ApiSession::generate_list_xml(const std::vector<std::string>& keys,
                                            const std::string& prefix,
                                            const std::string& continuation,
                                            size_t max_keys) const {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
        << "  <Name>" << "your-bucket" << "</Name>\n"  // placeholder
        << "  <Prefix>" << prefix << "</Prefix>\n"
        << "  <MaxKeys>" << max_keys << "</MaxKeys>\n"
        << "  <IsTruncated>false</IsTruncated>\n";

    for (const auto& k : keys) {
        oss << "  <Contents>\n"
            << "    <Key>" << k << "</Key>\n"
            << "    <LastModified>2026-01-01T00:00:00.000Z</LastModified>\n"
            << "    <ETag>\"placeholder\"</ETag>\n"
            << "    <Size>0</Size>\n"
            << "  </Contents>\n";
    }

    oss << "</ListBucketResult>";
    return oss.str();
}

void S3ApiSession::handle_complete_multipart(const std::string& bucket,
        const std::string& key,
        const std::string& upload_id) {
    // The body is XML like:
    // <?xml version="1.0" encoding="UTF-8"?>
    // <CompleteMultipartUpload xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
    //   <Part>
    //     <PartNumber>1</PartNumber>
    //     <ETag>"etag1"</ETag>
    //   </Part>
    //   <Part>
    //     <PartNumber>2</PartNumber>
    //     <ETag>"etag2"</ETag>
    //   </Part>
    //   ...
    // </CompleteMultipartUpload>

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(req_.body().c_str());

    if (!result) {
    send_error(http::status::bad_request, "InvalidXML");
    std::cerr << "XML parse error: " << result.description() << std::endl;
    return;
    }

    // Optional: Check root element
    pugi::xml_node root = doc.child("CompleteMultipartUpload");
    if (!root) {
    send_error(http::status::bad_request, "InvalidRootElement");
    return;
    }

    // Collect parts: vector<pair<part_number, etag>>
    std::vector<std::pair<int, std::string>> etags;

    for (pugi::xml_node part_node : root.children("Part")) {
    pugi::xml_node num_node   = part_node.child("PartNumber");
    pugi::xml_node etag_node  = part_node.child("ETag");

    if (!num_node || !etag_node) {
    send_error(http::status::bad_request, "MissingPartInfo");
    return;
    }

    std::string num_str = num_node.text().as_string();
    std::string etag    = etag_node.text().as_string();

    // Clean ETag: remove surrounding quotes if present (S3 often sends "etag")
    if (!etag.empty() && etag.front() == '"' && etag.back() == '"') {
    etag = etag.substr(1, etag.size() - 2);
    }

    try {
    int part_num = std::stoi(num_str);
    if (part_num < 1 || part_num > 10000) {
    send_error(http::status::bad_request, "InvalidPartNumber");
    return;
    }
    etags.emplace_back(part_num, etag);
    } catch (...) {
    send_error(http::status::bad_request, "InvalidPartNumberFormat");
    return;
    }
    }

    // Sort by part number (S3 requires parts in ascending order, but we validate)
    std::sort(etags.begin(), etags.end());

    // Call storage layer
    if (storage_.CompleteMultipartUpload(bucket, key, upload_id, etags)) {
    // Success response (minimal S3-like XML)
    http::response<http::string_body> res{http::status::ok, req_.version()};
    res.set(http::field::content_type, "application/xml");

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    << "<CompleteMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
    << "  <Location>http://localhost:9000/" << bucket << "/" << key << "</Location>\n"
    << "  <Bucket>" << bucket << "</Bucket>\n"
    << "  <Key>" << key << "</Key>\n"
    << "  <ETag>\"" << "computed-final-etag-placeholder" << "\"</ETag>\n"  // TODO: compute real ETag of final object
    << "</CompleteMultipartUploadResult>";

    res.body() = xml.str();
    res.prepare_payload();
    send_response(std::move(res));
    } else {
    send_error(http::status::bad_request, "InvalidPartOrderOrETagMismatch");
    }
}

void S3ApiSession::send_response(http::response<http::string_body>&& res) {
    res.version(req_.version());
    res.keep_alive(req_.keep_alive());
    http::async_write(stream_, res,
        beast::bind_front_handler(&S3ApiSession::on_write, shared_from_this()));
}

void S3ApiSession::send_error(http::status status, const std::string& msg) {
    http::response<http::string_body> res{status, req_.version()};
    res.set(http::field::content_type, "application/xml");
    std::string xml = "<Error><Code>" + msg + "</Code><Message>" + msg + "</Message></Error>";
    res.body() = xml;
    res.prepare_payload();
    send_response(std::move(res));
}

void S3ApiSession::on_write(beast::error_code ec, std::size_t) {
    if (!ec) do_read();  // support keep-alive
    // else close
}

// ────────────────────────────────────────────────

S3ApiServer::S3ApiServer(net::io_context& ioc, tcp::endpoint endpoint, StorageManager& storage)
    : ioc_(ioc), acceptor_(ioc), storage_(storage) {
    beast::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(endpoint, ec);
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
}

void S3ApiServer::run() {
    do_accept();
}

void S3ApiServer::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<S3ApiSession>(std::move(socket), storage_)->run();
            }
            do_accept();
        });
}








// Add a credentials map (in production: load from secure store, database, or config):



// // Dummy in-memory credentials store (replace with secure backend!)
// static const std::map<std::string, std::string> credentials = {
//     {"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"},
//     // Add more: access_key -> secret_key
// };



