// common/config_loader.cpp (simplified - add encryption/decryption in production with OpenSSL or similar)
#include "config_loader.h"
#include <fstream>
#include <iostream>
// Assume JSON parser like nlohmann/json or rapidjson is included

CloudConfig g_config;

bool LoadConfig(const std::string& path, const std::string& key) {
    // TODO: Decrypt file using key (HSM if available)
    std::ifstream file(path);
    if (!file) return false;

    // Parse example JSON (placeholder)
    // In real impl, decrypt then parse
    g_config.backend_ips = {"192.168.1.10:8080", "192.168.1.11:8080"};
    g_config.ca_cert_path = "/etc/certs/ca.pem";
    // ... load all fields

    std::cout << "Loaded config from " << path << std::endl;
    return true;
}