// scaler.cpp
void ScalingLoop() {
    while (true) {
        double cpu = collector.GetAvgCpu();
        if (cpu > g_config.cpu_threshold_high) {
            // Scale up: launch new instance (via script or API to hypervisor)
        } else if (cpu < g_config.cpu_threshold_low) {
            // Scale down
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}