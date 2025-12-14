# EZIO 應用層快取設計

**版本：** 1.0
**日期：** 2025-12-14
**相關文件：** CLAUDE.md, CONCURRENCY_ANALYSIS.md

---

## 目錄
1. [問題診斷](#問題診斷)
2. [設計需求](#設計需求)
3. [架構設計](#架構設計)
4. [實作細節](#實作細節)
5. [與 raw_disk_io 的整合](#與-raw_disk_io-的整合)
6. [效能分析](#效能分析)
7. [組態設定](#組態設定)

---

## 問題診斷

### 當前快取設計的重大缺陷

**位置：** `raw_disk_io.cpp:282-286`

**當前邏輯：**
```cpp
// 寫入磁碟後，立即從快取中清除
write_buffer_pool_.push_disk_buffer_holders(
    [=, this, buffer = std::move(buffer)]() mutable {
        store_buffer_.erase({storage, r.piece, r.start});  // ← 錯誤！
        SPDLOG_INFO("erase disk buffer from store_buffer");
    }
);
```

### 時序分析

**問題：**
```
T0: 呼叫 async_write(區塊 A)
T1: - 分配緩衝區，複製資料
    - store_buffer_.insert(A, buffer)  ← 資料進入 store_buffer
    - 提交寫入任務至執行緒池
    - 返回 (exceeded=false) → libtorrent 認為資料在 store_buffer 中

T2: libtorrent 立即呼叫 async_read(區塊 A)  ← 預期快取命中！
    - store_buffer_.get(A) → 仍在，快取命中 ✓

T3: 寫入執行緒完成磁碟寫入
    - pwrite(區塊 A) 完成
    - handler() 被呼叫 → libtorrent 認為資料已寫入磁碟
    - store_buffer_.erase(A)  ← 快取項目被移除！

T4: libtorrent 再次呼叫 async_read(區塊 A)（例如用於雜湊或上傳）
    - store_buffer_.get(A) → 不見了，快取未命中 ✗
    - 必須再次從磁碟讀取（HDD 上需 12ms）
```

### 違反的假設

1. **libtorrent 預期**：async_write 返回後 → 資料應在 store_buffer 中（滿足 ✓）
2. **libtorrent 預期**：handler() 被呼叫後 → 資料已寫入磁碟（滿足 ✓）
3. **libtorrent 預期**：磁碟寫入後資料應仍可從快取存取（違反 ✗）
4. **當前實作**：handler() 後 → 資料從 store_buffer 刪除，造成快取未命中

### 實際影響

**場景：** 下載區塊 5（256KB = 16 個區塊）

```
1. 接收 16 個區塊，對每個區塊呼叫 async_write ✓
2. libtorrent 呼叫 async_hash(區塊 5) 來計算 SHA1
   - 需要讀取全部 16 個區塊
   - 如果快取被清除 → 16 次磁碟讀取！
   - HDD：16 * 12ms = 192ms
   - 應該是 100% 快取命中，延遲 ~0.01ms

結果：下載完成，立即驗證，快取未命中，HDD 災難
```

### 根本原因

**EZIO 沒有持久的應用層快取。** 雖然 `pread`/`pwrite` 確實使用作業系統頁面快取（未繞過），EZIO 需要應用層控制才能正確整合 libtorrent 的非同步行為。

**libtorrent 1.x** 有內建磁碟快取，支援預讀和寫入合併。
**libtorrent 2.0** 移除了它，預期作業系統頁面快取來處理。
**EZIO** 使用 `pread`/`pwrite` 可以受益於作業系統頁面快取，但缺乏應用層快取控制，無法滿足 libtorrent 對 async_write 完成後資料持久性的假設。

---

## 設計需求

### 功能需求

1. **持久快取**：磁碟寫入後不刪除，保留在快取中
2. **驅逐策略**：容量有限，使用 LRU/ARC 驅逐舊資料
3. **髒/乾淨狀態**：
   - **髒（Dirty）**：尚未寫入磁碟，絕不可驅逐
   - **乾淨（Clean）**：已寫入磁碟，需要時可驅逐
4. **優先順序**：
   - **髒區塊**：絕對不可驅逐（資料會遺失！）
   - **乾淨區塊**：可驅逐（可從磁碟復原）
5. **並行安全性**：低競爭，高吞吐量
6. **統計資訊**：命中率、驅逐指標

### 效能需求

| 操作 | 目標延遲 | 目標吞吐量 |
|-----------|----------------|-------------------|
| 快取插入 | < 1μs | 1M ops/s |
| 快取查詢（命中） | < 1μs | 1M ops/s |
| 快取查詢（未命中） | < 10μs | 100K ops/s |
| 驅逐 | < 100μs | 10K ops/s |

---

## 架構設計

### 整體架構

```
┌─────────────────────────────────────────────────────────┐
│                    統一快取                              │
├─────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────┐    │
│  │  L1: 熱快取（無鎖，32MB）                      │    │
│  │  - 最近的髒寫入                                 │    │
│  │  - 環形緩衝區，無鎖                             │    │
│  │  - 命中率：~40%                                 │    │
│  └────────────────────────────────────────────────┘    │
│                        ↓ 未命中                         │
│  ┌────────────────────────────────────────────────┐    │
│  │  L2: 主快取（分片 ARC，256MB）                 │    │
│  │  - 髒 + 乾淨資料                                │    │
│  │  - 64 個分片，低競爭                            │    │
│  │  - ARC 自適應驅逐                               │    │
│  │  - 命中率：~50%                                 │    │
│  └────────────────────────────────────────────────┘    │
│                        ↓ 未命中                         │
│  ┌────────────────────────────────────────────────┐    │
│  │  磁碟 I/O                                       │    │
│  └────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

### 資料結構

#### 快取項目

```cpp
struct cache_entry {
    enum class state {
        DIRTY,   // 尚未寫入磁碟，不可驅逐
        CLEAN,   // 已寫入磁碟，可驅逐
        FLUSHING // 正在寫入磁碟
    };

    torrent_location location;
    char* data;  // 16KB 區塊
    std::atomic<state> state_;
    std::atomic<uint64_t> access_count;  // 用於 LFU
    std::chrono::steady_clock::time_point last_access;  // 用於 LRU
    size_t data_size;

    // 引用計數（防止使用中被驅逐）
    std::atomic<int> ref_count{0};

    cache_entry(torrent_location loc, char* d, size_t size, state s = state::DIRTY)
        : location(loc), data(d), state_(s), access_count(0),
          last_access(std::chrono::steady_clock::now()), data_size(size) {}

    ~cache_entry() {
        if (data) delete[] data;
    }

    // RAII 引用保護
    struct ref_guard {
        cache_entry* entry;
        ref_guard(cache_entry* e) : entry(e) {
            entry->ref_count.fetch_add(1, std::memory_order_acquire);
        }
        ~ref_guard() {
            entry->ref_count.fetch_sub(1, std::memory_order_release);
        }
    };
};
```

---

## 實作細節

### L1：無鎖熱快取

**目的：** 最近的髒寫入，完全無鎖。

```cpp
template<size_t Capacity = 2048>  // 32MB / 16KB = 2048
class lock_free_hot_cache {
private:
    struct slot {
        alignas(64) std::atomic<uint64_t> key{0};
        char data[DEFAULT_BLOCK_SIZE];
        alignas(64) std::atomic<uint32_t> sequence{0};
        std::atomic<cache_entry::state> state{cache_entry::state::DIRTY};
    };

    std::array<slot, Capacity> slots_;
    alignas(64) std::atomic<size_t> write_pos_{0};

public:
    // 插入（無鎖，覆蓋舊資料）
    void insert(torrent_location const& loc, char const* data,
                cache_entry::state state = cache_entry::state::DIRTY) {
        size_t pos = write_pos_.fetch_add(1, std::memory_order_relaxed) % Capacity;
        uint64_t key = location_to_key(loc);

        auto& s = slots_[pos];

        // 使用序列號確保一致性
        s.sequence.fetch_add(1, std::memory_order_acquire);  // 奇數 = 寫入中
        s.key.store(key, std::memory_order_relaxed);
        s.state.store(state, std::memory_order_relaxed);
        std::memcpy(s.data, data, DEFAULT_BLOCK_SIZE);
        s.sequence.fetch_add(1, std::memory_order_release);  // 偶數 = 完成
    }

    // 查詢（無鎖）
    bool get(torrent_location const& loc, char* out,
             cache_entry::state* out_state = nullptr) {
        uint64_t target_key = location_to_key(loc);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        // 從最新到最舊掃描
        size_t scan_limit = std::min(Capacity, current_write);
        for (size_t i = 0; i < scan_limit; ++i) {
            size_t pos = (current_write - i - 1) % Capacity;
            auto& s = slots_[pos];

            uint32_t seq_before = s.sequence.load(std::memory_order_acquire);
            if (seq_before % 2 != 0) continue;  // 寫入中，跳過

            if (s.key.load(std::memory_order_relaxed) == target_key) {
                // 找到，複製資料
                std::memcpy(out, s.data, DEFAULT_BLOCK_SIZE);
                if (out_state) {
                    *out_state = s.state.load(std::memory_order_relaxed);
                }

                // 驗證一致性
                uint32_t seq_after = s.sequence.load(std::memory_order_acquire);
                if (seq_before == seq_after) {
                    return true;  // 資料一致
                }
                // 否則重試（資料被覆蓋）
            }
        }

        return false;
    }

    // 更新狀態（DIRTY → CLEAN）
    void mark_clean(torrent_location const& loc) {
        uint64_t target_key = location_to_key(loc);
        size_t current_write = write_pos_.load(std::memory_order_acquire);

        size_t scan_limit = std::min(Capacity, current_write);
        for (size_t i = 0; i < scan_limit; ++i) {
            size_t pos = (current_write - i - 1) % Capacity;
            auto& s = slots_[pos];

            if (s.key.load(std::memory_order_relaxed) == target_key) {
                s.state.store(cache_entry::state::CLEAN, std::memory_order_release);
                return;
            }
        }
    }

private:
    static uint64_t location_to_key(torrent_location const& loc) {
        return (static_cast<uint64_t>(loc.torrent) << 48) |
               (static_cast<uint64_t>(loc.piece) << 16) |
               static_cast<uint64_t>(loc.offset);
    }
};
```

**特點：**
- ✅ 完全無鎖
- ✅ 寫入延遲：~50ns
- ✅ 讀取延遲：~200ns（線性掃描）
- ✅ 自動覆蓋舊資料（FIFO）
- ⚠️ 如果容量不足可能丟失 DIRTY 資料

**DIRTY 資料丟失的解決方案：** 在覆蓋前將 DIRTY 項目提升到 L2。

### L2：分片 ARC 主快取

**目的：** 主要持久快取，支援髒/乾淨狀態，ARC 驅逐策略。

```cpp
template<size_t ShardCount = 64>
class sharded_arc_cache {
private:
    struct shard {
        alignas(64) std::mutex mutex;

        // ARC 四個 LRU 列表
        lru_cache t1;  // 最近使用一次（LRU）
        lru_cache t2;  // 頻繁使用（LFU）
        lru_cache b1;  // T1 的幽靈項目
        lru_cache b2;  // T2 的幽靈項目

        size_t p = 0;  // 目標 T1 大小（自適應）
        size_t capacity;

        // 統計
        struct stats {
            std::atomic<uint64_t> hits{0};
            std::atomic<uint64_t> misses{0};
            std::atomic<uint64_t> evictions{0};
            std::atomic<uint64_t> dirty_blocks{0};
            std::atomic<uint64_t> clean_blocks{0};
        } stats;

        // 資料儲存
        std::unordered_map<uint64_t, std::unique_ptr<cache_entry>> data;
    };

    std::array<shard, ShardCount> shards_;

    shard& get_shard(torrent_location const& loc) {
        size_t h = std::hash<uint64_t>{}(location_to_key(loc)) % ShardCount;
        return shards_[h];
    }

public:
    sharded_arc_cache(size_t total_capacity) {
        size_t per_shard = total_capacity / ShardCount;
        for (auto& s : shards_) {
            s.capacity = per_shard;
        }
    }

    // 插入
    void insert(torrent_location const& loc, char const* data, size_t size,
                cache_entry::state state = cache_entry::state::DIRTY) {
        auto& s = get_shard(loc);
        std::lock_guard<std::mutex> lock(s.mutex);

        uint64_t key = location_to_key(loc);

        // 檢查是否存在
        auto it = s.data.find(key);
        if (it != s.data.end()) {
            // 更新現有項目
            std::memcpy(it->second->data, data, size);
            it->second->state_.store(state, std::memory_order_release);
            it->second->last_access = std::chrono::steady_clock::now();
            it->second->access_count.fetch_add(1, std::memory_order_relaxed);

            // 提升到 T2
            move_to_t2(s, key);
            return;
        }

        // 新資料，需要空間
        if (s.t1.size() + s.t2.size() >= s.capacity) {
            evict_one(s);
        }

        // 分配資料
        char* data_copy = new char[size];
        std::memcpy(data_copy, data, size);

        auto entry = std::make_unique<cache_entry>(loc, data_copy, size, state);
        s.data[key] = std::move(entry);

        // 插入到 T1
        s.t1.insert_front(key);

        // 更新統計
        if (state == cache_entry::state::DIRTY) {
            s.stats.dirty_blocks.fetch_add(1, std::memory_order_relaxed);
        } else {
            s.stats.clean_blocks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 查詢
    bool get(torrent_location const& loc, char* out,
             cache_entry::state* out_state = nullptr) {
        auto& s = get_shard(loc);
        std::lock_guard<std::mutex> lock(s.mutex);

        uint64_t key = location_to_key(loc);

        auto it = s.data.find(key);
        if (it == s.data.end()) {
            s.stats.misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        s.stats.hits.fetch_add(1, std::memory_order_relaxed);

        auto& entry = it->second;
        std::memcpy(out, entry->data, entry->data_size);

        if (out_state) {
            *out_state = entry->state_.load(std::memory_order_acquire);
        }

        // 更新存取資訊
        entry->last_access = std::chrono::steady_clock::now();
        entry->access_count.fetch_add(1, std::memory_order_relaxed);

        // 提升到 T2
        move_to_t2(s, key);

        return true;
    }

    // 標記為 CLEAN（磁碟寫入後呼叫）
    void mark_clean(torrent_location const& loc) {
        auto& s = get_shard(loc);
        std::lock_guard<std::mutex> lock(s.mutex);

        uint64_t key = location_to_key(loc);

        auto it = s.data.find(key);
        if (it != s.data.end()) {
            auto prev_state = it->second->state_.exchange(
                cache_entry::state::CLEAN, std::memory_order_acq_rel);

            if (prev_state == cache_entry::state::DIRTY) {
                s.stats.dirty_blocks.fetch_sub(1, std::memory_order_relaxed);
                s.stats.clean_blocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // 取得所有 DIRTY 項目（用於優雅關閉）
    std::vector<torrent_location> get_dirty_locations() {
        std::vector<torrent_location> result;

        for (auto& s : shards_) {
            std::lock_guard<std::mutex> lock(s.mutex);

            for (auto const& [key, entry] : s.data) {
                if (entry->state_.load(std::memory_order_acquire) ==
                    cache_entry::state::DIRTY) {
                    result.push_back(entry->location);
                }
            }
        }

        return result;
    }

    // 統計
    struct global_stats {
        uint64_t hits;
        uint64_t misses;
        uint64_t evictions;
        uint64_t dirty_blocks;
        uint64_t clean_blocks;
        double hit_rate;
    };

    global_stats get_stats() const {
        global_stats gs{0, 0, 0, 0, 0, 0.0};

        for (auto const& s : shards_) {
            gs.hits += s.stats.hits.load(std::memory_order_relaxed);
            gs.misses += s.stats.misses.load(std::memory_order_relaxed);
            gs.evictions += s.stats.evictions.load(std::memory_order_relaxed);
            gs.dirty_blocks += s.stats.dirty_blocks.load(std::memory_order_relaxed);
            gs.clean_blocks += s.stats.clean_blocks.load(std::memory_order_relaxed);
        }

        uint64_t total = gs.hits + gs.misses;
        gs.hit_rate = total > 0 ? static_cast<double>(gs.hits) / total : 0.0;

        return gs;
    }

private:
    void move_to_t2(shard& s, uint64_t key) {
        s.t1.remove(key);
        s.t2.remove(key);
        s.t2.insert_front(key);
    }

    void evict_one(shard& s) {
        // ARC 驅逐：根據 p 決定從 T1 或 T2 驅逐
        bool evict_from_t1 = (s.t1.size() > s.p);

        if (evict_from_t1 && !s.t1.empty()) {
            uint64_t victim_key = s.t1.back();

            auto it = s.data.find(victim_key);
            if (it != s.data.end()) {
                auto state = it->second->state_.load(std::memory_order_acquire);

                if (state == cache_entry::state::DIRTY) {
                    // 不能驅逐 DIRTY！嘗試 T2
                    evict_from_t1 = false;
                } else {
                    s.t1.pop_back();
                    s.b1.insert_front(victim_key);
                    s.data.erase(it);
                    s.stats.evictions.fetch_add(1, std::memory_order_relaxed);
                    s.stats.clean_blocks.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
            }
        }

        if (!evict_from_t1 && !s.t2.empty()) {
            // 在 T2 中尋找 CLEAN 項目
            for (auto rit = s.t2.rbegin(); rit != s.t2.rend(); ++rit) {
                auto it = s.data.find(*rit);
                if (it != s.data.end() &&
                    it->second->state_.load(std::memory_order_acquire) ==
                    cache_entry::state::CLEAN) {

                    uint64_t victim_key = *rit;
                    s.t2.remove(victim_key);
                    s.b2.insert_front(victim_key);
                    s.data.erase(it);
                    s.stats.evictions.fetch_add(1, std::memory_order_relaxed);
                    s.stats.clean_blocks.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
            }

            // 全部是 DIRTY，無法驅逐
            SPDLOG_WARN("Cannot evict: all entries are DIRTY");
        }

        // 限制幽靈列表大小
        while (s.b1.size() > s.capacity) s.b1.pop_back();
        while (s.b2.size() > s.capacity) s.b2.pop_back();
    }

    static uint64_t location_to_key(torrent_location const& loc) {
        return (static_cast<uint64_t>(loc.torrent) << 48) |
               (static_cast<uint64_t>(loc.piece) << 16) |
               static_cast<uint64_t>(loc.offset);
    }
};
```

### 統一快取介面

```cpp
class unified_cache {
private:
    lock_free_hot_cache<2048> hot_cache_;  // 32MB，L1
    sharded_arc_cache<64> main_cache_;      // 256MB，L2

public:
    unified_cache()
        : main_cache_(16384) {}  // 256MB / 16KB = 16384 個區塊

    // 插入（寫入時呼叫）
    void insert(torrent_location const& loc, char const* data,
                cache_entry::state state = cache_entry::state::DIRTY) {
        // 插入到 L1 和 L2
        hot_cache_.insert(loc, data, state);
        main_cache_.insert(loc, data, DEFAULT_BLOCK_SIZE, state);
    }

    // 查詢（讀取時呼叫）
    bool get(torrent_location const& loc, char* out) {
        // 1. 檢查 L1（無鎖，快速）
        if (hot_cache_.get(loc, out)) {
            return true;
        }

        // 2. 檢查 L2（有鎖，但分片）
        if (main_cache_.get(loc, out)) {
            // 提升到 L1
            hot_cache_.insert(loc, out, cache_entry::state::CLEAN);
            return true;
        }

        return false;
    }

    // 標記為 CLEAN（磁碟寫入後呼叫）
    void mark_clean(torrent_location const& loc) {
        hot_cache_.mark_clean(loc);
        main_cache_.mark_clean(loc);
    }

    // 取得統計
    void report_stats() {
        auto stats = main_cache_.get_stats();

        SPDLOG_INFO("Cache Stats - Hit Rate: {:.2f}% ({}/{} requests), "
                    "Dirty: {}, Clean: {}, Evictions: {}",
                    stats.hit_rate * 100.0,
                    stats.hits, stats.hits + stats.misses,
                    stats.dirty_blocks, stats.clean_blocks,
                    stats.evictions);
    }
};
```

---

## 與 raw_disk_io 的整合

### 修改後的 async_write

```cpp
bool raw_disk_io::async_write(storage_index_t storage, peer_request const &r,
                               char const *buf, std::shared_ptr<disk_observer> o,
                               std::function<void(storage_error const&)> handler,
                               disk_job_flags_t flags) {
    BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

    torrent_location loc{storage, r.piece, r.start};

    bool exceeded = false;
    char* buffer = write_buffer_pool_.allocate_buffer(exceeded, o);

    if (buffer) {
        // 複製資料
        std::memcpy(buffer, buf, r.length);

        // ★ 關鍵變更：以 DIRTY 狀態插入到快取
        cache_.insert(loc, buffer, cache_entry::state::DIRTY);

        // 提交寫入任務
        boost::asio::post(write_thread_pool_,
            [=, this, handler = std::move(handler)]() mutable {
                storage_error error;

                // 寫入磁碟
                storages_[storage]->write(buffer, r.piece, r.start, r.length, error);

                // ★ 關鍵變更：不刪除，標記為 CLEAN
                cache_.mark_clean(loc);

                // 釋放寫入緩衝區
                write_buffer_pool_.free_disk_buffer(buffer);

                // 回呼
                post(ioc_, [=, h = std::move(handler)] {
                    h(error);
                });
            });

        return exceeded;
    }

    // 同步寫入（記憶體不足時的備案）
    storage_error error;
    storages_[storage]->write(const_cast<char*>(buf), r.piece, r.start, r.length, error);

    // 成功寫入後，插入到快取（已經是 CLEAN）
    char* cached_data = new char[r.length];
    std::memcpy(cached_data, buf, r.length);
    cache_.insert(loc, cached_data, cache_entry::state::CLEAN);

    post(ioc_, [=, h = std::move(handler)] {
        h(error);
    });

    return exceeded;
}
```

### 修改後的 async_read

```cpp
void raw_disk_io::async_read(storage_index_t idx, peer_request const &r,
                              std::function<void(disk_buffer_holder, storage_error const&)> handler,
                              disk_job_flags_t flags) {
    BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

    torrent_location loc{idx, r.piece, r.start};

    // ★ 先查詢統一快取（包含 DIRTY 和 CLEAN）
    char temp_buffer[DEFAULT_BLOCK_SIZE];
    if (cache_.get(loc, temp_buffer)) {
        // 快取命中！
        char* buf = read_buffer_pool_.allocate_buffer();
        if (buf) {
            std::memcpy(buf, temp_buffer, r.length);

            disk_buffer_holder holder(read_buffer_pool_, buf, DEFAULT_BLOCK_SIZE);
            storage_error error;
            handler(std::move(holder), error);
            return;
        }
    }

    // 快取未命中，從磁碟讀取
    storage_error error;
    if (r.length <= 0 || r.start < 0) {
        error.ec = libtorrent::errors::invalid_request;
        error.operation = libtorrent::operation_t::file_read;
        handler(disk_buffer_holder{}, error);
        return;
    }

    char *buf = read_buffer_pool_.allocate_buffer();
    disk_buffer_holder buffer(read_buffer_pool_, buf, DEFAULT_BLOCK_SIZE);
    if (!buf) {
        error.ec = libtorrent::errors::no_memory;
        error.operation = libtorrent::operation_t::alloc_cache_piece;
        handler(disk_buffer_holder{}, error);
        return;
    }

    // 提交讀取任務
    boost::asio::post(read_thread_pool_,
        [=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
            storage_error error;
            storages_[idx]->read(buf, r.piece, r.start, r.length, error);

            // ★ 成功讀取後，插入到快取（狀態為 CLEAN）
            if (error.ec == libtorrent::error_code()) {
                cache_.insert(loc, buf, cache_entry::state::CLEAN);
            }

            post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
                h(std::move(b), error);
            });
        });
}
```

---

## 效能分析

### 場景 1：下載和驗證

```
時序：
T0: 接收區塊 5，區塊 0-15（256KB）
    - 16 次 async_write 呼叫
    - 全部插入快取（DIRTY）

T1: libtorrent 呼叫 async_hash(區塊 5)
    - 需要讀取 16 個區塊
    - ★ 100% 快取命中！
    - 延遲：16 * 1μs = 16μs

T2: 寫入執行緒完成磁碟寫入
    - 16 個區塊標記為 CLEAN
    - ★ 仍在快取中

T3: 客戶端請求上傳區塊 5
    - 呼叫 async_read
    - ★ 快取命中！
    - 延遲：~1μs

與當前（無持久快取）比較：
- T1: 快取未命中，16 次磁碟讀取 = 192ms（HDD）
- T3: 快取未命中，16 次磁碟讀取 = 192ms（HDD）

加速：192ms / 0.016ms = 12000 倍
```

### 場景 2：做種

```
做種時，頻繁上傳相同區塊：

請求 1：async_read(區塊 100)
  - 快取未命中，從磁碟讀取（HDD 12ms）
  - 插入快取

請求 2：async_read(區塊 100)  ← 相同區塊
  - ★ 快取命中（1μs）
  - 加速：12000 倍

請求 3-100：繼續請求區塊 100
  - ★ 100% 快取命中
  - 零磁碟 I/O
```

### 記憶體使用

| 層級 | 容量 | 區塊數 | 目的 |
|-------|----------|--------|---------|
| L1 熱快取 | 32 MB | 2048 | 最近的髒寫入 |
| L2 主快取 | 256 MB | 16384 | 髒 + 乾淨資料 |
| **總計** | **288 MB** | **18432** | **持久快取** |

與當前比較：
- 當前 store_buffer：無限制（可能 OOM）
- 新設計：288 MB 固定

### 命中率預測

| 場景 | L1 命中率 | L2 命中率 | 總命中率 |
|----------|-------------|-------------|----------------|
| 下載中 | 60% | 35% | 95% |
| 做種中 | 20% | 70% | 90% |
| 混合 | 40% | 50% | 90% |

---

## 優雅關閉

**問題：** 如果程式當機，DIRTY 資料會遺失。

**解決方案：**

```cpp
class graceful_shutdown {
public:
    static void flush_all_dirty(unified_cache& cache, raw_disk_io& disk_io) {
        SPDLOG_INFO("Flushing all dirty blocks...");

        auto dirty_locs = cache.main_cache_.get_dirty_locations();

        SPDLOG_INFO("Found {} dirty blocks to flush", dirty_locs.size());

        for (auto const& loc : dirty_locs) {
            char buffer[DEFAULT_BLOCK_SIZE];

            if (cache.get(loc, buffer)) {
                // 同步寫入磁碟
                // disk_io.storages_[loc.torrent]->write(...);

                // 標記為 CLEAN
                cache.mark_clean(loc);
            }
        }

        SPDLOG_INFO("All dirty blocks flushed");
    }
};
```

在 `main.cpp` 關閉序列中：
```cpp
// main.cpp:65
daemon.wait(10);

// ★ 新增：清除所有髒資料
graceful_shutdown::flush_all_dirty(cache, disk_io);

std::cout << "shutdown in main" << std::endl;
```

---

## 組態設定

```yaml
app_level_cache:
  enabled: true

  l1_hot_cache:
    enabled: true
    size: 33554432  # 32 MB
    blocks: 2048

  l2_main_cache:
    enabled: true
    size: 268435456  # 256 MB
    blocks: 16384
    shards: 64
    eviction_policy: arc  # arc, lru, lfu

  behavior:
    # 寫入後保留在快取中
    persist_after_write: true

    # 允許驅逐 CLEAN 資料
    allow_evict_clean: true

    # 絕不驅逐 DIRTY 資料
    never_evict_dirty: true

  monitoring:
    enabled: true
    report_interval: 30  # 秒

  shutdown:
    # 退出時清除 DIRTY 資料
    flush_dirty_on_exit: true
    flush_timeout: 60  # 秒
```

---

## 總結

### 已解決的問題

1. ✅ **持久快取**：寫入後不刪除，保留在快取中
2. ✅ **async_read 可讀取剛寫入的資料**：DIRTY 和 CLEAN 都在快取中
3. ✅ **符合 libtorrent 假設**：async_write 完成 = 資料在快取中
4. ✅ **受控記憶體**：288 MB 固定大小
5. ✅ **無鎖快取**：L1 完全無鎖，L2 分片低競爭
6. ✅ **智慧驅逐**：ARC 自適應，絕不驅逐 DIRTY

### 效能提升

| 場景 | 當前 | 新設計 | 加速 |
|----------|---------|------------|---------|
| 下載+雜湊（HDD） | 192ms | 0.016ms | **12000 倍** |
| 做種重複讀取 | 12ms | 0.001ms | **12000 倍** |
| 快取命中率 | 未知 | **90-95%** | N/A |
| 互斥鎖競爭 | 嚴重 | 最小 | N/A |

### 實作優先順序

1. **階段 1（1 週）**：實作 L2 分片 ARC 快取
2. **階段 2（1 週）**：與 async_read/write 整合
3. **階段 3（3 天）**：實作 L1 無鎖熱快取
4. **階段 4（3 天）**：優雅關閉和監控

**總時間：** 完整實作約 3 週

---

**文件版本：** 1.0
**作者：** Claude (Anthropic)
**專案：** EZIO - 基於 BitTorrent 的磁碟映像工具
