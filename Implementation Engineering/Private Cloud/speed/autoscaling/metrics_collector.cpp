









metrics->hot_object_count = hot_index_->GetHotCount();




// In metrics_collector.cpp
metrics->dedup_bloom_memory_mb = bloom_->memory_usage_bytes() / 1024 / 1024;
metrics->dedup_bloom_false_positives++;  // increment when Bloom yes but LevelDB no










