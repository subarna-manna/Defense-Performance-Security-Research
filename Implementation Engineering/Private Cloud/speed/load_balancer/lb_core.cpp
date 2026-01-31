// speed/load_balancer/lb_core.cpp
#include "lb_core.h"
#include "lb_config.h"  // for defines
#include <iostream>
#include <unistd.h>  // for sleep

LoadBalancer::LoadBalancer() {
    // Load from config
    for (const auto& ip : g_config.backend_ips) {
        backends_.push_back({ip, true, 0});
    }
}

LoadBalancer::~LoadBalancer() {
    if (health_thread_.joinable()) health_thread_.join();
}

void LoadBalancer::AddBackend(const std::string& addr) {
    backends_.push_back({addr, true, 0});
}

Backend* LoadBalancer::GetNextHealthyBackend() {
    size_t size = backends_.size();
    if (size == 0) return nullptr;

    for (size_t i = 0; i < size; ++i) {
        size_t idx = rr_index_.fetch_add(1) % size;
        if (backends_[idx].healthy.load()) {
            return &backends_[idx];
        }
    }
    return nullptr;  // all unhealthy
}

void LoadBalancer::HealthCheckLoop() {
    while (true) {
        for (auto& b : backends_) {
            // TODO: Real HTTP/gRPC health check to /health
            // For now, simulate
            bool ok = (rand() % 10 != 0);  // 90% healthy
            b.healthy.store(ok);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.healthcheck_interval_ms));
    }
}

void LoadBalancer::StartHealthChecks() {
    health_thread_ = std::thread(&LoadBalancer::HealthCheckLoop, this);
}