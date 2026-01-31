// data/fast_access/lru_cache.h
#include <list>
#include <unordered_map>

template<typename K, typename V>
class LRUCache {
private:
    size_t capacity_;
    std::list<std::pair<K, V>> list_;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
public:
    LRUCache(size_t cap) : capacity_(cap) {}
    V* Get(const K& key);
    void Put(const K& key, V value);
    // Evict least recently used
};
// /* key_store.c */
// int hsm_generate_key(const char* label, unsigned char** key_out, size_t* len);
// int hsm_sign(const unsigned char* data, size_t len, unsigned char** sig, size_t* sig_len);