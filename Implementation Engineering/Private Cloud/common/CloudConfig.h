
bool enable_dedup_gc = true;
int gc_interval_hours = 24;
int gc_grace_period_hours = 48;     // future: check deletion timestamp
int gc_batch_size = 1000;

// In struct CloudConfig
bool enable_bloom_filter = true;
size_t bloom_expected_items = 100000000;  // 100 million chunks
double bloom_false_positive_rate = 0.01;



