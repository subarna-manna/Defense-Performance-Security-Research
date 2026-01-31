#include "s3_api_server.h"
#include "storage_manager.h"
#include "config_loader.h"
#include <iostream>

int main() {
    if (!LoadConfig("config.enc", "key")) {
        std::cerr << "Config load failed\n";
        return 1;
    }

    StorageManager storage;

    net::io_context ioc{std::thread::hardware_concurrency()};

    tcp::endpoint endpoint{tcp::v4(), 9000};  // From g_config if desired

    auto server = std::make_shared<S3ApiServer>(ioc, endpoint, storage);
    server->run();

    std::cout << "S3-compatible API listening on :9000\n";

    std::vector<std::thread> threads;
    for (auto i = 0u; i < std::thread::hardware_concurrency(); ++i) {
        threads.emplace_back([&ioc] { ioc.run(); });
    }
    for (auto& t : threads) t.join();

    return 0;
}