# 快取大小配置實作指南

## 摘要

本文件說明如何讓 EZIO 的 buffer pool 快取大小可配置，將設定從 EZIO 配置傳遞到 libtorrent 的 settings_pack，再傳遞到 raw_disk_io。

**目前狀態：** `settings_updated()` 存在但是**空的**（位於 raw_disk_io.cpp 的第 464-466 行）

**需要的變更：**
1. 在 `raw_disk_io` 中儲存 `settings_interface&` 參考
2. 實作 `settings_updated()` 以更新 buffer pool 大小
3. 加入配置傳遞路徑
4. 正確處理統一的 buffer pool

---

## 目前架構的問題

### 問題 1：settings_interface 沒有被儲存

**位置：** `raw_disk_io.cpp:114-119`

```cpp
std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(
    libtorrent::io_context &ioc,
    libtorrent::settings_interface const &s,  // ← 收到但被忽略！
    libtorrent::counters &c)                   // ← 也被忽略！
{
    return std::make_unique<raw_disk_io>(ioc);  // ← 只傳遞 ioc！
}
```

**問題：** Settings 和 counters 被丟棄了

### 問題 2：settings_updated() 是空的

**位置：** `raw_disk_io.cpp:464-466`

```cpp
void raw_disk_io::settings_updated()
{
    // 空的！應該在這裡更新 buffer pool 大小
}
```

**問題：** 當設定改變時沒有執行任何動作

### 問題 3：buffer_pool 沒有 set_settings()

**位置：** `buffer_pool.hpp`

```cpp
template<typename Fun>
class buffer_pool : public libtorrent::buffer_allocator_interface
{
    // 沒有 set_settings() 方法！
    // 快取大小在 MAX_BUFFER_POOL_SIZE 中寫死
};
```

**問題：** 無法動態調整快取大小

---

## libtorrent 的參考實作

### mmap_disk_io (libtorrent 2.x)

**建構子** (`mmap_disk_io.cpp:389-400`):

```cpp
mmap_disk_io::mmap_disk_io(io_context& ios,
                            settings_interface const& sett,  // ← 被儲存！
                            counters& cnt)
    : m_buffer_pool(ios)
    , m_settings(sett)        // ← 儲存參考
    , m_stats_counters(cnt)   // ← 儲存參考
    // ...
{
    // 使用目前設定初始化
    settings_updated();
}
```

**settings_updated()** (`mmap_disk_io.cpp:498-510`):

```cpp
void mmap_disk_io::settings_updated()
{
    // 更新 buffer pool
    m_buffer_pool.set_settings(m_settings);

    // 更新 file pool
    m_file_pool.resize(m_settings.get_int(settings_pack::file_pool_size));

    // 更新執行緒池
    int const num_threads = m_settings.get_int(settings_pack::aio_threads);
    int const num_hash_threads = m_settings.get_int(settings_pack::hashing_threads);
    m_generic_threads.set_max_threads(num_threads);
    m_hash_threads.set_max_threads(num_hash_threads);
}
```

**disk_buffer_pool::set_settings()** (`disk_buffer_pool.cpp:198-213`):

```cpp
void disk_buffer_pool::set_settings(settings_interface const& sett)
{
    std::unique_lock<std::mutex> l(m_pool_mutex);

    // 從設定計算 pool 大小
    int const pool_size = std::max(1,
        sett.get_int(settings_pack::max_queued_disk_bytes) / default_block_size);

    m_max_use = pool_size;
    m_low_watermark = m_max_use / 2;

    // 如果已經超過則觸發 watermark
    if (m_in_use >= m_max_use && !m_exceeded_max_size)
    {
        m_exceeded_max_size = true;
    }
}
```

---

## 實作計畫

### 步驟 1：更新 raw_disk_io 類別

**檔案：** `raw_disk_io.hpp`

**加入成員變數：**

```cpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    // 現有成員
    buffer_pool read_buffer_pool_;   // 將變成 unified_buffer_pool_
    buffer_pool write_buffer_pool_;  // 將被移除
    store_buffer store_buffer_;

    // 新增：儲存 settings 參考
    libtorrent::settings_interface const& settings_;

    // 新增：儲存 counters 參考（供未來統計使用）
    libtorrent::counters& stats_counters_;

    libtorrent::io_context& ioc_;
    // ...

public:
    // 更新：建構子簽名
    raw_disk_io(libtorrent::io_context& ioc,
                libtorrent::settings_interface const& sett,
                libtorrent::counters& cnt);

    // 現有方法...
    void settings_updated() override;  // 將正確實作
};
```

### 步驟 2：更新 raw_disk_io_constructor

**檔案：** `raw_disk_io.cpp`

**更新工廠函式：**

```cpp
std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(
    libtorrent::io_context &ioc,
    libtorrent::settings_interface const &sett,
    libtorrent::counters &cnt)
{
    // 之前：
    // return std::make_unique<raw_disk_io>(ioc);

    // 之後：
    return std::make_unique<raw_disk_io>(ioc, sett, cnt);
}
```

### 步驟 3：更新 raw_disk_io 建構子

**檔案：** `raw_disk_io.cpp`

**更新建構子：**

```cpp
// 之前：
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc)
    : ioc_(ioc)
    , read_buffer_pool_(ioc)
    , write_buffer_pool_(ioc)
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
}

// 之後：
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
                          libtorrent::settings_interface const& sett,
                          libtorrent::counters& cnt)
    : settings_(sett)                      // 儲存參考
    , stats_counters_(cnt)                 // 儲存參考
    , ioc_(ioc)
    , read_buffer_pool_(ioc)               // 或 unified_buffer_pool_
    , write_buffer_pool_(ioc)              // 合併後移除
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
    // 套用初始設定
    settings_updated();
}
```

### 步驟 4：加入 buffer_pool::set_settings()

**檔案：** `buffer_pool.hpp`

**加入方法宣告：**

```cpp
template<typename Fun>
class buffer_pool : public libtorrent::buffer_allocator_interface
{
public:
    // 現有方法...
    char* allocate_buffer();
    void free_disk_buffer(char*) override;

    // 新增：配置 pool 大小
    void set_settings(libtorrent::settings_interface const& sett);

private:
    int m_size;               // 目前已配置數量
    int m_max_size;           // 新增：最大大小（可配置）
    int m_low_watermark;      // 新增：低水位
    int m_high_watermark;     // 新增：高水位
    bool m_exceeded_max_size;
    // ...
};
```

**加入實作（在 buffer_pool.hpp 中，因為它是 template）：**

```cpp
template<typename Fun>
void buffer_pool<Fun>::set_settings(libtorrent::settings_interface const& sett)
{
    std::unique_lock<std::mutex> l(m_pool_mutex);

    // 從設定取得快取大小
    size_t const cache_bytes = sett.get_int(
        libtorrent::settings_pack::max_queued_disk_bytes);

    // 計算區塊數量
    size_t const new_max_size = cache_bytes / DEFAULT_BLOCK_SIZE;

    // 更新限制
    m_max_size = new_max_size;
    m_low_watermark = new_max_size / 2;              // 50%
    m_high_watermark = new_max_size / 8 * 7;         // 87.5%

    // 檢查是否現在已超過
    if (m_size >= m_max_size && !m_exceeded_max_size)
    {
        m_exceeded_max_size = true;
        // 通知觀察者
        check_buffer_level(l);
    }
}
```

### 步驟 5：實作 settings_updated()

**檔案：** `raw_disk_io.cpp`

**實作方法：**

```cpp
void raw_disk_io::settings_updated()
{
    // 使用新設定更新 buffer pools
    read_buffer_pool_.set_settings(settings_);
    write_buffer_pool_.set_settings(settings_);

    // 注意：buffer pool 合併後，這會變成：
    // unified_buffer_pool_.set_settings(settings_);

    // 如果需要，也可以更新執行緒池大小：
    // int num_threads = settings_.get_int(settings_pack::aio_threads);
    // read_thread_pool_.set_max_threads(num_threads);
    // write_thread_pool_.set_max_threads(num_threads);
}
```

---

## 配置流程

### 完整路徑：EZIO 配置 → libtorrent → raw_disk_io

**1. 使用者配置（例如：命令列、配置檔）**

```cpp
// 在 main.cpp 或配置載入處：
libtorrent::settings_pack sett;

// 設定快取大小（位元組）
sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
             256 * 1024 * 1024);  // 256 MB

// 套用到 session
session.apply_settings(sett);
```

**2. libtorrent Session**

當呼叫 `apply_settings()` 時，libtorrent：
- 內部儲存設定
- 呼叫 `m_disk_thread->settings_updated()`

**3. raw_disk_io::settings_updated()**

```cpp
void raw_disk_io::settings_updated()
{
    // 讀取 settings_.get_int(settings_pack::max_queued_disk_bytes)
    // 傳遞到 buffer_pool
    unified_buffer_pool_.set_settings(settings_);
}
```

**4. buffer_pool::set_settings()**

```cpp
void buffer_pool::set_settings(...)
{
    // 計算新的 max_size
    // 更新 watermarks
    // 檢查是否超過
}
```

### 範例：動態配置變更

```cpp
// 使用者在執行期間改變快取大小：
libtorrent::settings_pack new_sett;
new_sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 512 * 1024 * 1024);  // 增加到 512 MB

session.apply_settings(new_sett);

// libtorrent 自動呼叫：
// → session_impl::apply_settings_pack()
// → m_disk_thread->settings_updated()
// → raw_disk_io::settings_updated()
// → unified_buffer_pool_.set_settings()
```

---

## 與 Buffer Pool 合併的整合

### 統一 Pool 實作之後

**檔案：** `raw_disk_io.hpp`

```cpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    libtorrent::settings_interface const& settings_;
    libtorrent::counters& stats_counters_;
    libtorrent::io_context& ioc_;

    // 統一的 pool（取代 read_ 和 write_buffer_pool_）
    buffer_pool unified_buffer_pool_;

    store_buffer store_buffer_;
    // ...
};
```

**檔案：** `raw_disk_io.cpp`

```cpp
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
                          libtorrent::settings_interface const& sett,
                          libtorrent::counters& cnt)
    : settings_(sett)
    , stats_counters_(cnt)
    , ioc_(ioc)
    , unified_buffer_pool_(ioc)  // 單一 pool
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
    settings_updated();  // 套用初始設定
}

void raw_disk_io::settings_updated()
{
    // 更新統一的 pool
    unified_buffer_pool_.set_settings(settings_);

    // 可選：更新執行緒池大小
    // ...
}
```

---

## 關於每執行緒快取（回應使用者問題）

### 使用者問題

> "### 實作 1.2：每執行緒快取
> 這個可能要考慮，async_read會去讀跟他不一樣thread的cache? 還是說這是實作細節，你已經幫我避開這個問題?"

翻譯：「實作 1.2：每執行緒快取 - 需要考慮 async_read 是否會讀取與它不同執行緒的快取？或這是實作細節，你已經幫我避開這個問題了？」

### 回答

**目前 EZIO 設計：共享全域快取（正確做法）**

EZIO 的 `store_buffer_` 是**單一全域快取**，由所有執行緒共享：

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    store_buffer store_buffer_;  // ← 被所有執行緒共享！
    // ...
};
```

**執行緒安全性：**

`store_buffer` 已經透過 mutex 保護來確保執行緒安全：

```cpp
// store_buffer.hpp
class store_buffer {
    template<typename Fun>
    bool get(torrent_location const loc, Fun f) {
        std::unique_lock<std::mutex> l(m_mutex);  // ← Mutex 保護
        auto const it = m_store_buffer.find(loc);
        if (it != m_store_buffer.end()) {
            f(it->second);
            return true;
        }
        return false;
    }

    void insert(torrent_location const loc, char const* buf) {
        std::lock_guard<std::mutex> l(m_mutex);  // ← Mutex 保護
        m_store_buffer.insert({loc, buf});
    }
private:
    std::mutex m_mutex;  // ← 保護所有操作
    std::unordered_map<torrent_location, char const*> m_store_buffer;
};
```

**行為：**

```
寫入執行緒 1：
    async_write(piece=5, offset=0)
    → store_buffer_.insert({storage, 5, 0}, buf)
    → 提交 pwrite 工作

讀取執行緒 2：
    async_read(piece=5, offset=0)
    → store_buffer_.get({storage, 5, 0}, ...)
    → 快取命中！回傳由執行緒 1 寫入的 buffer ✓
```

### 每執行緒快取：不建議

**為何不使用每執行緒快取：**

❌ **跨執行緒存取問題：**
```
寫入執行緒 1：將 piece 5 寫入執行緒 1 的快取
讀取執行緒 2：嘗試讀取 piece 5 → 未命中！（在執行緒 1 的快取中）
結果：即使資料在記憶體中仍必須從磁碟讀取
```

❌ **記憶體浪費：**
- 相同的 piece 可能存在於多個執行緒快取中
- 總記憶體 = 執行緒數 × 快取大小

❌ **複雜度：**
- 需要快取一致性協定
- 需要執行緒到快取的親和性
- libtorrent 不保證哪個執行緒服務哪個 piece

### 建議

**保持目前設計：** 單一共享的 `store_buffer` 搭配 mutex 保護。

**為何有效：**

✅ **任何執行緒都可以存取任何快取的區塊**
✅ **沒有快取重複**
✅ **簡單且正確**
✅ **符合 libtorrent 2.x 的設計**

**效能：**

Mutex 競爭是最小的，因為：
1. 快取查找非常快（雜湊表 O(1)）
2. 臨界區很短
3. 大部分時間花在實際的磁碟 I/O（在 mutex 外）

如果效能分析顯示 mutex 競爭：
- 使用 `std::shared_mutex`（讀寫鎖）
  - 多個讀取者可以同時存取
  - 只有寫入者會阻塞
- 或使用無鎖雜湊表（例如 `folly::ConcurrentHashMap`）

**結論：** 目前的實作是正確的。每執行緒快取會引入問題而沒有明確的好處。

---

## 測試計畫

### 單元測試

**測試 1：Settings 傳遞**

```cpp
TEST(raw_disk_io, settings_propagation)
{
    libtorrent::io_context ioc;
    libtorrent::settings_pack sett;
    libtorrent::counters cnt;

    // 設定 512 MB 快取
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 512 * 1024 * 1024);

    auto dio = std::make_unique<raw_disk_io>(ioc, sett, cnt);

    // 驗證 buffer pool 正確配置
    // （需要為 m_max_size 加入 getter）
    EXPECT_EQ(dio->get_buffer_pool_size(), 512 * 1024 * 1024);
}
```

**測試 2：動態 Settings 更新**

```cpp
TEST(raw_disk_io, dynamic_settings_update)
{
    libtorrent::io_context ioc;
    MockSettingsInterface sett;
    libtorrent::counters cnt;

    // 初始：256 MB
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 256 * 1024 * 1024);

    auto dio = std::make_unique<raw_disk_io>(ioc, sett, cnt);
    EXPECT_EQ(dio->get_buffer_pool_size(), 256 * 1024 * 1024);

    // 更新：128 MB
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 128 * 1024 * 1024);
    dio->settings_updated();

    EXPECT_EQ(dio->get_buffer_pool_size(), 128 * 1024 * 1024);
}
```

### 整合測試

**測試 3：端到端配置**

```cpp
TEST(session, cache_size_configuration)
{
    libtorrent::session_params params;
    params.disk_io_constructor = ezio::raw_disk_io_constructor;

    libtorrent::settings_pack sett;
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 1024 * 1024 * 1024);  // 1 GB
    params.settings = sett;

    libtorrent::session ses(params);

    // 驗證快取大小反映在磁碟 I/O 中
    // （需要 session API 來查詢磁碟 I/O 統計）
}
```

---

## 遷移檢查清單

### 階段 1：加入 Settings 支援（本文件）

- [ ] 更新 `raw_disk_io.hpp` 以儲存 `settings_` 參考
- [ ] 更新 `raw_disk_io_constructor()` 以傳遞 settings
- [ ] 更新 `raw_disk_io` 建構子以接受 settings
- [ ] 加入 `buffer_pool::set_settings()` 方法
- [ ] 實作 `raw_disk_io::settings_updated()`
- [ ] 測試 settings 傳遞
- [ ] 測試動態 settings 更新

### 階段 2：合併 Buffer Pools（平行進行）

- [ ] 實作統一的 buffer pool（參見 BUFFER_POOL_MERGER.md）
- [ ] 為統一 pool 更新 `settings_updated()`
- [ ] 測試統一 pool 與可配置大小

### 階段 3：文件

- [ ] 更新使用者文件，加入快取配置範例
- [ ] 加入配置建議（快取大小指南）
- [ ] 記錄不同快取大小的效能影響

---

## 配置建議

### 快取大小指南

**最小值：** 64 MB
- 低於此值會有太多快取未命中
- HDD 上效能不佳

**建議值：** 256-512 MB
- 大多數工作負載的良好平衡
- EZIO 預設：256 MB（128 MB × 2 pools）

**最大值：** 1-2 GB
- 超過此值的報酬遞減
- 有記憶體壓力的風險

**公式：**

```
cache_size = min(
    system_memory * 0.25,  // 系統 RAM 的 25%
    max(
        256 MB,            // 最小值
        num_torrents * 64 MB  // 隨 torrents 數量擴展
    )
)
```

**配置範例：**

```cpp
// 小型系統（4 GB RAM，1 個 torrent）：
sett.set_int(settings_pack::max_queued_disk_bytes, 256 * 1024 * 1024);

// 中型系統（16 GB RAM，5 個 torrents）：
sett.set_int(settings_pack::max_queued_disk_bytes, 512 * 1024 * 1024);

// 大型系統（64 GB RAM，20 個 torrents）：
sett.set_int(settings_pack::max_queued_disk_bytes, 2048 * 1024 * 1024);
```

---

## 結論

實作可配置快取大小需要：

1. **儲存 settings 參考**在 `raw_disk_io` 中
2. **實作 `settings_updated()`** 以傳遞變更
3. **加入 `buffer_pool::set_settings()`** 以處理動態大小調整
4. **保持共享全域快取**（不要每執行緒）

**工作量：** 約 1 天實作 + 測試

**風險：** 低（settings 系統在 libtorrent 中已建立完善）

**好處：** 對於具有不同工作負載的生產部署至關重要
