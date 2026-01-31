// speed/autoscaling/metrics_collector.h
class MetricsCollector {
    public:
        double GetAvgCpu();
        double GetLatency();
        // Collect from /proc or Prometheus exporter
    };