# Cache Size Configuration Implementation Guide

## Executive Summary

This document explains how to make EZIO's buffer pool cache size configurable, propagating settings from EZIO configuration through libtorrent's settings_pack to raw_disk_io.

**Current Status:** `settings_updated()` exists but is **empty** (line 464-466 in raw_disk_io.cpp)

**Required Changes:**
1. Store `settings_interface&` reference in `raw_disk_io`
2. Implement `settings_updated()` to update buffer pool sizes
3. Add configuration propagation path
4. Handle unified buffer pool properly

---

## Current Architecture Problems

### Problem 1: settings_interface Not Stored

**Location:** `raw_disk_io.cpp:114-119`

```cpp
std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(
    libtorrent::io_context &ioc,
    libtorrent::settings_interface const &s,  // ← Received but ignored!
    libtorrent::counters &c)                   // ← Also ignored!
{
    return std::make_unique<raw_disk_io>(ioc);  // ← Only passes ioc!
}
```

**Problem:** Settings and counters are discarded

### Problem 2: Empty settings_updated()

**Location:** `raw_disk_io.cpp:464-466`

```cpp
void raw_disk_io::settings_updated()
{
    // Empty! Should update buffer pool sizes here
}
```

**Problem:** No action taken when settings change

### Problem 3: buffer_pool Has No set_settings()

**Location:** `buffer_pool.hpp`

```cpp
template<typename Fun>
class buffer_pool : public libtorrent::buffer_allocator_interface
{
    // No set_settings() method!
    // Cache size is hardcoded in MAX_BUFFER_POOL_SIZE
};
```

**Problem:** Cannot dynamically adjust cache size

---

## libtorrent's Reference Implementation

### mmap_disk_io (libtorrent 2.x)

**Constructor** (`mmap_disk_io.cpp:389-400`):

```cpp
mmap_disk_io::mmap_disk_io(io_context& ios,
                            settings_interface const& sett,  // ← Stored!
                            counters& cnt)
    : m_buffer_pool(ios)
    , m_settings(sett)        // ← Store reference
    , m_stats_counters(cnt)   // ← Store reference
    // ...
{
    // Initialize with current settings
    settings_updated();
}
```

**settings_updated()** (`mmap_disk_io.cpp:498-510`):

```cpp
void mmap_disk_io::settings_updated()
{
    // Update buffer pool
    m_buffer_pool.set_settings(m_settings);

    // Update file pool
    m_file_pool.resize(m_settings.get_int(settings_pack::file_pool_size));

    // Update thread pools
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

    // Calculate pool size from setting
    int const pool_size = std::max(1,
        sett.get_int(settings_pack::max_queued_disk_bytes) / default_block_size);

    m_max_use = pool_size;
    m_low_watermark = m_max_use / 2;

    // Trigger watermark if already exceeded
    if (m_in_use >= m_max_use && !m_exceeded_max_size)
    {
        m_exceeded_max_size = true;
    }
}
```

---

## Implementation Plan

### Step 1: Update raw_disk_io Class

**File:** `raw_disk_io.hpp`

**Add members:**

```cpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    // Existing members
    buffer_pool read_buffer_pool_;   // Will become unified_buffer_pool_
    buffer_pool write_buffer_pool_;  // Will be removed
    store_buffer store_buffer_;

    // NEW: Store settings reference
    libtorrent::settings_interface const& settings_;

    // NEW: Store counters reference (for future stats)
    libtorrent::counters& stats_counters_;

    libtorrent::io_context& ioc_;
    // ...

public:
    // UPDATE: Constructor signature
    raw_disk_io(libtorrent::io_context& ioc,
                libtorrent::settings_interface const& sett,
                libtorrent::counters& cnt);

    // Existing methods...
    void settings_updated() override;  // Will implement properly
};
```

### Step 2: Update raw_disk_io_constructor

**File:** `raw_disk_io.cpp`

**Update factory function:**

```cpp
std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(
    libtorrent::io_context &ioc,
    libtorrent::settings_interface const &sett,
    libtorrent::counters &cnt)
{
    // BEFORE:
    // return std::make_unique<raw_disk_io>(ioc);

    // AFTER:
    return std::make_unique<raw_disk_io>(ioc, sett, cnt);
}
```

### Step 3: Update raw_disk_io Constructor

**File:** `raw_disk_io.cpp`

**Update constructor:**

```cpp
// BEFORE:
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc)
    : ioc_(ioc)
    , read_buffer_pool_(ioc)
    , write_buffer_pool_(ioc)
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
}

// AFTER:
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
                          libtorrent::settings_interface const& sett,
                          libtorrent::counters& cnt)
    : settings_(sett)                      // Store reference
    , stats_counters_(cnt)                 // Store reference
    , ioc_(ioc)
    , read_buffer_pool_(ioc)               // Or unified_buffer_pool_
    , write_buffer_pool_(ioc)              // Remove after merge
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
    // Apply initial settings
    settings_updated();
}
```

### Step 4: Add buffer_pool::set_settings()

**File:** `buffer_pool.hpp`

**Add method declaration:**

```cpp
template<typename Fun>
class buffer_pool : public libtorrent::buffer_allocator_interface
{
public:
    // Existing methods...
    char* allocate_buffer();
    void free_disk_buffer(char*) override;

    // NEW: Configure pool size
    void set_settings(libtorrent::settings_interface const& sett);

private:
    int m_size;               // Current allocated count
    int m_max_size;           // NEW: Max size (configurable)
    int m_low_watermark;      // NEW: Low watermark
    int m_high_watermark;     // NEW: High watermark
    bool m_exceeded_max_size;
    // ...
};
```

**Add implementation (in buffer_pool.hpp since it's a template):**

```cpp
template<typename Fun>
void buffer_pool<Fun>::set_settings(libtorrent::settings_interface const& sett)
{
    std::unique_lock<std::mutex> l(m_pool_mutex);

    // Get cache size from settings
    size_t const cache_bytes = sett.get_int(
        libtorrent::settings_pack::max_queued_disk_bytes);

    // Calculate number of blocks
    size_t const new_max_size = cache_bytes / DEFAULT_BLOCK_SIZE;

    // Update limits
    m_max_size = new_max_size;
    m_low_watermark = new_max_size / 2;              // 50%
    m_high_watermark = new_max_size / 8 * 7;         // 87.5%

    // Check if now exceeded
    if (m_size >= m_max_size && !m_exceeded_max_size)
    {
        m_exceeded_max_size = true;
        // Notify observers
        check_buffer_level(l);
    }
}
```

### Step 5: Implement settings_updated()

**File:** `raw_disk_io.cpp`

**Implement the method:**

```cpp
void raw_disk_io::settings_updated()
{
    // Update buffer pools with new settings
    read_buffer_pool_.set_settings(settings_);
    write_buffer_pool_.set_settings(settings_);

    // NOTE: After buffer pool merger, this becomes:
    // unified_buffer_pool_.set_settings(settings_);

    // Could also update thread pool sizes if desired:
    // int num_threads = settings_.get_int(settings_pack::aio_threads);
    // read_thread_pool_.set_max_threads(num_threads);
    // write_thread_pool_.set_max_threads(num_threads);
}
```

---

## Configuration Flow

### Complete Path: EZIO Config → libtorrent → raw_disk_io

**1. User Configuration (e.g., command line, config file)**

```cpp
// In main.cpp or config loading:
libtorrent::settings_pack sett;

// Set cache size (in bytes)
sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
             256 * 1024 * 1024);  // 256 MB

// Apply to session
session.apply_settings(sett);
```

**2. libtorrent Session**

When `apply_settings()` is called, libtorrent:
- Stores settings internally
- Calls `m_disk_thread->settings_updated()`

**3. raw_disk_io::settings_updated()**

```cpp
void raw_disk_io::settings_updated()
{
    // Reads settings_.get_int(settings_pack::max_queued_disk_bytes)
    // Propagates to buffer_pool
    unified_buffer_pool_.set_settings(settings_);
}
```

**4. buffer_pool::set_settings()**

```cpp
void buffer_pool::set_settings(...)
{
    // Calculates new max_size
    // Updates watermarks
    // Checks if exceeded
}
```

### Example: Dynamic Configuration Change

```cpp
// User changes cache size at runtime:
libtorrent::settings_pack new_sett;
new_sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 512 * 1024 * 1024);  // Increase to 512 MB

session.apply_settings(new_sett);

// libtorrent automatically calls:
// → session_impl::apply_settings_pack()
// → m_disk_thread->settings_updated()
// → raw_disk_io::settings_updated()
// → unified_buffer_pool_.set_settings()
```

---

## Integration with Buffer Pool Merger

### After Unified Pool Implementation

**File:** `raw_disk_io.hpp`

```cpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    libtorrent::settings_interface const& settings_;
    libtorrent::counters& stats_counters_;
    libtorrent::io_context& ioc_;

    // Unified pool (replaces read_ and write_buffer_pool_)
    buffer_pool unified_buffer_pool_;

    store_buffer store_buffer_;
    // ...
};
```

**File:** `raw_disk_io.cpp`

```cpp
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
                          libtorrent::settings_interface const& sett,
                          libtorrent::counters& cnt)
    : settings_(sett)
    , stats_counters_(cnt)
    , ioc_(ioc)
    , unified_buffer_pool_(ioc)  // Single pool
    , read_thread_pool_(8)
    , write_thread_pool_(8)
    , hash_thread_pool_(8)
{
    settings_updated();  // Apply initial settings
}

void raw_disk_io::settings_updated()
{
    // Update unified pool
    unified_buffer_pool_.set_settings(settings_);

    // Optional: Update thread pool sizes
    // ...
}
```

---

## About Per-Thread Cache (Regarding User Question)

### User Question

> "### 實作 1.2：每執行緒快取
> 這個可能要考慮，async_read會去讀跟他不一樣thread的cache? 還是說這是實作細節，你已經幫我避開這個問題?"

Translation: "Implementation 1.2: Per-thread cache - Need to consider if async_read will read from a different thread's cache? Or is this an implementation detail you've already handled?"

### Answer

**Current EZIO Design: Shared Global Cache (Correct)**

EZIO's `store_buffer_` is a **single global cache** shared by all threads:

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    store_buffer store_buffer_;  // ← Shared by all threads!
    // ...
};
```

**Thread Safety:**

`store_buffer` is already thread-safe with mutex protection:

```cpp
// store_buffer.hpp
class store_buffer {
    template<typename Fun>
    bool get(torrent_location const loc, Fun f) {
        std::unique_lock<std::mutex> l(m_mutex);  // ← Mutex protection
        auto const it = m_store_buffer.find(loc);
        if (it != m_store_buffer.end()) {
            f(it->second);
            return true;
        }
        return false;
    }

    void insert(torrent_location const loc, char const* buf) {
        std::lock_guard<std::mutex> l(m_mutex);  // ← Mutex protection
        m_store_buffer.insert({loc, buf});
    }
private:
    std::mutex m_mutex;  // ← Protects all operations
    std::unordered_map<torrent_location, char const*> m_store_buffer;
};
```

**Behavior:**

```
Write Thread 1:
    async_write(piece=5, offset=0)
    → store_buffer_.insert({storage, 5, 0}, buf)
    → Submit pwrite job

Read Thread 2:
    async_read(piece=5, offset=0)
    → store_buffer_.get({storage, 5, 0}, ...)
    → Cache HIT! Returns buffer written by Thread 1 ✓
```

### Per-Thread Cache: Not Recommended

**Why Not Use Per-Thread Cache:**

❌ **Cross-thread access problem:**
```
Write Thread 1: Writes piece 5 to Thread 1's cache
Read Thread 2: Tries to read piece 5 → MISS! (in Thread 1's cache)
Result: Must read from disk even though data is in memory
```

❌ **Memory waste:**
- Same piece may exist in multiple thread caches
- Total memory = num_threads × cache_size

❌ **Complexity:**
- Need cache coherency protocol
- Need thread-to-cache affinity
- libtorrent doesn't guarantee which thread serves which piece

### Recommendation

**Keep current design:** Single shared `store_buffer` with mutex protection.

**Why it works:**

✅ **Any thread can access any cached block**
✅ **No cache duplication**
✅ **Simple and correct**
✅ **Matches libtorrent 2.x design**

**Performance:**

Mutex contention is minimal because:
1. Cache lookups are very fast (hash table O(1))
2. Critical section is short
3. Most time spent in actual disk I/O (outside mutex)

If profiling shows mutex contention:
- Use `std::shared_mutex` (read-write lock)
  - Multiple readers can access simultaneously
  - Only writers block
- Or use lock-free hash table (e.g., `folly::ConcurrentHashMap`)

**Conclusion:** Current implementation is correct. Per-thread cache would introduce problems without clear benefits.

---

## Testing Plan

### Unit Tests

**Test 1: Settings Propagation**

```cpp
TEST(raw_disk_io, settings_propagation)
{
    libtorrent::io_context ioc;
    libtorrent::settings_pack sett;
    libtorrent::counters cnt;

    // Set 512 MB cache
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 512 * 1024 * 1024);

    auto dio = std::make_unique<raw_disk_io>(ioc, sett, cnt);

    // Verify buffer pool configured correctly
    // (Would need to add getter for m_max_size)
    EXPECT_EQ(dio->get_buffer_pool_size(), 512 * 1024 * 1024);
}
```

**Test 2: Dynamic Settings Update**

```cpp
TEST(raw_disk_io, dynamic_settings_update)
{
    libtorrent::io_context ioc;
    MockSettingsInterface sett;
    libtorrent::counters cnt;

    // Initial: 256 MB
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 256 * 1024 * 1024);

    auto dio = std::make_unique<raw_disk_io>(ioc, sett, cnt);
    EXPECT_EQ(dio->get_buffer_pool_size(), 256 * 1024 * 1024);

    // Update: 128 MB
    sett.set_int(libtorrent::settings_pack::max_queued_disk_bytes,
                 128 * 1024 * 1024);
    dio->settings_updated();

    EXPECT_EQ(dio->get_buffer_pool_size(), 128 * 1024 * 1024);
}
```

### Integration Tests

**Test 3: End-to-End Configuration**

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

    // Verify cache size reflected in disk I/O
    // (Would need session API to query disk I/O stats)
}
```

---

## Migration Checklist

### Phase 1: Add Settings Support (This Document)

- [ ] Update `raw_disk_io.hpp` to store `settings_` reference
- [ ] Update `raw_disk_io_constructor()` to pass settings
- [ ] Update `raw_disk_io` constructor to accept settings
- [ ] Add `buffer_pool::set_settings()` method
- [ ] Implement `raw_disk_io::settings_updated()`
- [ ] Test settings propagation
- [ ] Test dynamic settings updates

### Phase 2: Merge Buffer Pools (Parallel)

- [ ] Implement unified buffer pool (see BUFFER_POOL_MERGER.md)
- [ ] Update `settings_updated()` for unified pool
- [ ] Test unified pool with configurable size

### Phase 3: Documentation

- [ ] Update user documentation with cache configuration examples
- [ ] Add configuration recommendations (cache size guidelines)
- [ ] Document performance impact of different cache sizes

---

## Configuration Recommendations

### Cache Size Guidelines

**Minimum:** 64 MB
- Below this, too many cache misses
- Poor performance on HDD

**Recommended:** 256-512 MB
- Good balance for most workloads
- EZIO default: 256 MB (128 MB × 2 pools)

**Maximum:** 1-2 GB
- Diminishing returns beyond this
- Risk of memory pressure

**Formula:**

```
cache_size = min(
    system_memory * 0.25,  // 25% of system RAM
    max(
        256 MB,            // Minimum
        num_torrents * 64 MB  // Scale with torrents
    )
)
```

**Example Configurations:**

```cpp
// Small system (4 GB RAM, 1 torrent):
sett.set_int(settings_pack::max_queued_disk_bytes, 256 * 1024 * 1024);

// Medium system (16 GB RAM, 5 torrents):
sett.set_int(settings_pack::max_queued_disk_bytes, 512 * 1024 * 1024);

// Large system (64 GB RAM, 20 torrents):
sett.set_int(settings_pack::max_queued_disk_bytes, 2048 * 1024 * 1024);
```

---

## Conclusion

Implementing configurable cache size requires:

1. **Store settings reference** in `raw_disk_io`
2. **Implement `settings_updated()`** to propagate changes
3. **Add `buffer_pool::set_settings()`** to handle dynamic sizing
4. **Keep shared global cache** (not per-thread)

**Effort:** ~1 day implementation + testing

**Risk:** Low (settings system well-established in libtorrent)

**Benefit:** Essential for production deployment with varying workloads
