// speed/load_balancer/lb_core.h
#ifndef LB_CORE_H
#define LB_CORE_H

#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>

struct Backend {
    std::string addr;  // "ip:port"
    std::atomic<bool> healthy{true};
    std::atomic<int> active_connections{0};
};

class LoadBalancer {
private:
    std::vector<Backend> backends_;
    std::atomic<size_t> rr_index_{0};
    std::thread health_thread_;

    void HealthCheckLoop();

public:
    LoadBalancer();
    ~LoadBalancer();

    void AddBackend(const std::string& addr);
    Backend* GetNextHealthyBackend();  // Round-robin among healthy
    void StartHealthChecks();
};

#endif