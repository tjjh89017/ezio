# EZIO Optimization Phase Plan (Reformulated)

**Version:** 2.0 (Complete Re-analysis)
**Date:** 2025-12-14
**Status:** Ready for Implementation
**Language:** English

---

## Executive Summary

This document presents a **reformulated phase plan** based on complete re-analysis of:
- All documentation in `docs/`
- Current code state (after Phase 0 completion)
- Dependencies and priorities
- Implementation complexity

**Key Changes from Original CLAUDE.md:**
- ✅ Resolved Phase 1.1 naming conflict
- ✅ Clearer dependency ordering
- ✅ Split complex phases into sub-phases
- ✅ Added effort estimates and success criteria
- ✅ Identified "quick wins" vs. long-term optimizations

---

## Phase Status

### ✅ Phase 0: Logging & Debugging (COMPLETED)

**Status:** Merged to master

**Phase 0.1: spdlog Runtime Log Control**
- Replaced 24 `SPDLOG_*` macros with `spdlog::*` functions
- Added runtime configuration via `SPDLOG_LEVEL` environment variable
- Updated README.md with documentation
- **Benefit:** Debug without recompiling

**Phase 0.2: Event-Driven Alert Handling**
- Implemented `set_alert_notify()` callback
- Replaced 5-second polling with condition_variable
- Instant alert response (<1ms vs 0-5s delay)
- **Benefit:** 5000x faster alert response

**Commit IDs:**
- Phase 0.1: `df30a4a`
- Phase 0.2: `bccea62`
- Documentation: `b62a1e7`

---

## Remaining Phases (Reformulated)

### 📋 Phase 1: Foundation Improvements (Memory & Configuration)

**Goal:** Fix foundational issues before adding complex optimizations

**Priority:** HIGH (blocking Phase 2)

**Estimated Duration:** 3-4 days total

---

#### Phase 1.1: Buffer Pool Merger

**What:** Merge `read_buffer_pool_` + `write_buffer_pool_` → `m_buffer_pool`

**Why First:**
- ✅ Simplest foundational change
- ✅ No complex logic, mostly mechanical refactoring
- ✅ Aligns with libtorrent 2.x design
- ✅ Blocks Phase 1.2 (config needs unified pool)

**Current State:**
```cpp
// raw_disk_io.hpp:24-25
buffer_pool read_buffer_pool_;   // 128 MB
buffer_pool write_buffer_pool_;  // 128 MB
store_buffer store_buffer_;      // Separate tracking
```

**Target State:**
```cpp
// raw_disk_io.hpp (after merge)
buffer_pool m_buffer_pool;       // 256 MB unified (m_ prefix)
store_buffer m_store_buffer;     // m_ prefix for consistency
```

**Changes Required:**

1. **buffer_pool.hpp** (1 file):
   ```cpp
   -#define MAX_BUFFER_POOL_SIZE (128ULL * 1024 * 1024)
   +#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)  // Increased for 10Gbps
   ```

2. **raw_disk_io.hpp** (1 file):
   ```cpp
   -buffer_pool read_buffer_pool_;
   -buffer_pool write_buffer_pool_;
   -store_buffer store_buffer_;
   +buffer_pool m_buffer_pool;      // Unified, m_ prefix
   +store_buffer m_store_buffer;    // m_ prefix
   ```

3. **raw_disk_io.cpp** (1 file):
   - Replace all `read_buffer_pool_` → `m_buffer_pool`
   - Replace all `write_buffer_pool_` → `m_buffer_pool`
   - Replace all `store_buffer_` → `m_store_buffer`
   - Update constructor initialization

**Affected Functions:**
- `raw_disk_io::raw_disk_io()` (constructor)
- `raw_disk_io::async_read()` (line 167-257)
- `raw_disk_io::async_write()` (line 259-297)
- `raw_disk_io::async_hash()` (line 299-349)

**Testing:**
1. Unit tests: Verify allocation/deallocation works
2. Integration: Run full download/upload cycle
3. Performance: Measure memory usage in unbalanced workloads

**Success Criteria:**
- ✅ All tests pass
- ✅ No performance regression
- ✅ Memory utilization: +40% for unbalanced workloads
- ✅ Code compiles without warnings

**Effort:** 1-2 days

**Documentation:** `docs/BUFFER_POOL_MERGER.md`

**Dependencies:** None (can start immediately)

---

#### Phase 1.2: Configurable Cache Size

**What:** Implement `settings_updated()` and make buffer pool size configurable

**Why After 1.1:**
- ❌ Cannot configure split pools (which one to resize?)
- ✅ Unified pool makes configuration straightforward
- ✅ Production requirement (different deployments need different sizes)

**Current State:**
```cpp
// raw_disk_io.cpp:114-119
std::unique_ptr<disk_interface> raw_disk_io_constructor(
    io_context& ioc,
    settings_interface const& s,  // ← Received but not used!
    counters& c)
{
    return std::make_unique<raw_disk_io>(ioc);  // ← Not passed!
}

// raw_disk_io.cpp:121-129 - Constructor
raw_disk_io::raw_disk_io(libtorrent::io_context &ioc) :
    ioc_(ioc),
    read_buffer_pool_(ioc),
    write_buffer_pool_(ioc),
    read_thread_pool_(8),     // ← Hardcoded!
    write_thread_pool_(8),    // ← Hardcoded!
    hash_thread_pool_(8)      // ← Hardcoded!
{
}

// raw_disk_io.cpp:464-466
void raw_disk_io::settings_updated() {
    // Empty!
}
```

**Target State (Following libtorrent 2.x Pattern):**
```cpp
// Constructor receives settings and configures from settings
raw_disk_io::raw_disk_io(io_context& ioc,
                          settings_interface const& sett,
                          counters& cnt)
    : ioc_(ioc),
      m_settings(&sett),                                              // ← Save reference
      m_stats_counters(cnt),                                          // ← Save reference
      m_buffer_pool(ioc),                                             // ← Only io_context (like libtorrent)
      read_thread_pool_(sett.get_int(settings_pack::aio_threads)),   // ← From settings!
      write_thread_pool_(sett.get_int(settings_pack::aio_threads)),  // ← From settings!
      hash_thread_pool_(sett.get_int(settings_pack::hashing_threads)) // ← From settings!
{
    // Do NOT call settings_updated() here
    // Let external caller (session or libtorrent) call it when needed
}

// Implement settings propagation for runtime updates
void raw_disk_io::settings_updated() {
    // Update buffer pool configuration
    m_buffer_pool.set_settings(*m_settings);

    // Update thread pool sizes dynamically
    int aio_threads = m_settings->get_int(settings_pack::aio_threads);
    read_thread_pool_.set_max_threads(aio_threads);
    write_thread_pool_.set_max_threads(aio_threads);

    int hash_threads = m_settings->get_int(settings_pack::hashing_threads);
    hash_thread_pool_.set_max_threads(hash_threads);
}
```

**Key Design Decisions:**

1. **Constructor does NOT call settings_updated()**
   - Only saves settings reference
   - Configures thread pools from settings in init list
   - buffer_pool uses default values initially (like libtorrent)

2. **settings_updated() for runtime updates**
   - Called by external code (session/libtorrent)
   - Updates buffer_pool configuration
   - Resizes thread pools dynamically

3. **Thread pool configuration**
   - Read from settings in initialization list
   - Uses `settings_pack::aio_threads` for I/O threads
   - Uses `settings_pack::hashing_threads` for hash threads

**Changes Required:**

1. **raw_disk_io.hpp** (add members):
   ```cpp
   class raw_disk_io final : public disk_interface {
   public:
       // Update constructor signature
   -   raw_disk_io(io_context&);
   +   raw_disk_io(io_context&, settings_interface const&, counters&);

   private:
   +   settings_interface const* m_settings;  // Configuration reference
   +   counters& m_stats_counters;            // Statistics reference
   };
   ```

2. **raw_disk_io.cpp** (modify constructor):
   ```cpp
   // raw_disk_io_constructor - pass through all parameters
   std::unique_ptr<disk_interface> raw_disk_io_constructor(
       io_context& ioc,
       settings_interface const& s,
       counters& c)
   {
   -   return std::make_unique<raw_disk_io>(ioc);
   +   return std::make_unique<raw_disk_io>(ioc, s, c);
   }

   // raw_disk_io::raw_disk_io - read from settings in init list
   -raw_disk_io::raw_disk_io(io_context& ioc)
   +raw_disk_io::raw_disk_io(io_context& ioc,
   +                          settings_interface const& sett,
   +                          counters& cnt)
       : ioc_(ioc),
   +     m_settings(&sett),                                             // ← Save reference
   +     m_stats_counters(cnt),                                         // ← Save reference
        m_buffer_pool(ioc),                                             // ← No change
   -    read_thread_pool_(8),
   -    write_thread_pool_(8),
   -    hash_thread_pool_(8)
   +    read_thread_pool_(sett.get_int(settings_pack::aio_threads)),   // ← From settings!
   +    write_thread_pool_(sett.get_int(settings_pack::aio_threads)),  // ← From settings!
   +    hash_thread_pool_(sett.get_int(settings_pack::hashing_threads)) // ← From settings!
   {
   +    // Do NOT call settings_updated() here
   }
   ```

3. **buffer_pool.hpp** (add method):
   ```cpp
   class buffer_pool : public libtorrent::buffer_allocator_interface {
   public:
   +    void set_settings(settings_interface const& sett);  // ← New method
   };
   ```

4. **buffer_pool.cpp** (implement set_settings):
   ```cpp
   +void buffer_pool::set_settings(settings_interface const& sett) {
   +    std::unique_lock<std::mutex> l(m_pool_mutex);
   +
   +    // Read max_queued_disk_bytes setting
   +    int pool_size = std::max(1,
   +        sett.get_int(settings_pack::max_queued_disk_bytes) / DEFAULT_BLOCK_SIZE);
   +
   +    // Update limits (cannot shrink below current usage)
   +    if (pool_size < m_size) {
   +        // Log warning but don't fail
   +        spdlog::warn("Cannot shrink buffer pool below current usage: {} < {}",
   +                     pool_size, m_size);
   +        return;
   +    }
   +
   +    // Update configuration
   +    // Note: BUFFER_COUNT macro is replaced by dynamic m_max_use
   +    m_max_use = pool_size;
   +    m_low_watermark = m_max_use / 2;
   +    m_high_watermark = m_low_watermark + (m_max_use - m_low_watermark) * 3 / 4;
   +
   +    // Re-check watermark state
   +    if (m_size >= m_high_watermark && !m_exceeded_max_size) {
   +        m_exceeded_max_size = true;
   +    } else if (m_size < m_low_watermark && m_exceeded_max_size) {
   +        m_exceeded_max_size = false;
   +        // Notify observers
   +        check_buffer_level(l);
   +    }
   +}
   ```

5. **raw_disk_io.cpp** (implement settings_updated):
   ```cpp
   void raw_disk_io::settings_updated() {
       // Update buffer pool configuration
       m_buffer_pool.set_settings(*m_settings);

       // Update thread pool sizes dynamically
       int aio_threads = m_settings->get_int(settings_pack::aio_threads);
       read_thread_pool_.set_max_threads(aio_threads);
       write_thread_pool_.set_max_threads(aio_threads);

       int hash_threads = m_settings->get_int(settings_pack::hashing_threads);
       hash_thread_pool_.set_max_threads(hash_threads);
   }
   ```

6. **buffer_pool.hpp** (add members for dynamic limits):
   ```cpp
   class buffer_pool {
   private:
       std::mutex m_pool_mutex;
       int m_size;                   // Current allocated blocks
   +   int m_max_use;                // Maximum blocks (was BUFFER_COUNT macro)
   +   int m_low_watermark;          // 50% of m_max_use
   +   int m_high_watermark;         // 87.5% of m_max_use
       bool m_exceeded_max_size;
       std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;
   };
   ```

7. **buffer_pool.cpp** (initialize in constructor):
   ```cpp
   buffer_pool::buffer_pool(libtorrent::io_context &ioc) :
       m_ios(ioc),
       m_size(0),
   +   m_max_use(BUFFER_COUNT),              // Default from macro
   +   m_low_watermark(LOW_WATERMARK),       // Default from macro
   +   m_high_watermark(HIGH_WATERMARK),     // Default from macro
       m_exceeded_max_size(false)
   {
   }
   ```

**Testing:**
1. Set `max_queued_disk_bytes` to different values
2. Verify buffer pool adjusts size
3. Ensure no crashes when changing at runtime
4. Test with values: 64MB, 128MB, 256MB, 512MB

**Success Criteria:**
- ✅ Buffer pool size responds to settings changes
- ✅ settings_updated() called by libtorrent
- ✅ No memory leaks
- ✅ Configuration persists across restarts

**Effort:** 1 day

**Documentation:** `docs/CACHE_SIZE_CONFIG.md`

**Dependencies:** Phase 1.1 (requires unified pool)

---

### 📋 Phase 2: Parallel Write Optimization (Simple)

**Goal:** Saturate NVMe queue depth with parallel writes

**Priority:** HIGH (simple change, major performance gain for NVMe)

**Estimated Duration:** 1 day

**Why Simple:** Just increase thread pool size, no complex logic needed

---

**What:** Increase write thread pool size from settings to saturate NVMe queue depth

**Why After Phase 1:**
- ✅ Need Phase 1.2 (settings infrastructure)
- ✅ Thread pool size already configured from settings in Phase 1.2
- ✅ No additional code needed!

**Current State (After Phase 1.2):**
```cpp
// Already done in Phase 1.2!
raw_disk_io::raw_disk_io(io_context& ioc,
                          settings_interface const& sett,
                          counters& cnt)
    : write_thread_pool_(sett.get_int(settings_pack::aio_threads)),  // ← Already from settings!
      ...
```

**What's Actually Needed:**

Just adjust the **settings value** at runtime:

```cpp
// In your application or session
lt::settings_pack pack;
pack.set_int(lt::settings_pack::aio_threads, 32);  // ← Increase for NVMe!
session.apply_settings(pack);
```

**Or detect storage type automatically:**

```cpp
// Optional enhancement: Auto-detect and configure
storage_type detect_storage_type(std::string const& path) {
    // Method 1: Check /sys/block/*/queue/rotational
    std::ifstream f("/sys/block/" + get_block_device(path) + "/queue/rotational");
    int rotational;
    if (f >> rotational) {
        if (rotational == 1) return storage_type::HDD;
    }

    // Method 2: Check device name
    if (path.find("/dev/nvme") == 0) return storage_type::SSD_NVME;

    // Default: SATA SSD
    return storage_type::SSD_SATA;
}

// Configure based on storage type
size_t get_optimal_threads(storage_type type) {
    switch (type) {
    case storage_type::HDD:
        return 2;    // HDD serializes anyway
    case storage_type::SSD_SATA:
        return 8;    // Moderate parallelism
    case storage_type::SSD_NVME:
        return 32;   // High parallelism
    }
}

// Apply configuration
lt::settings_pack pack;
storage_type type = detect_storage_type("/dev/sda1");
pack.set_int(lt::settings_pack::aio_threads, get_optimal_threads(type));
session.apply_settings(pack);
```

**Why This is Phase 2:**

1. **Already implemented in Phase 1.2**
   - Constructor reads from settings
   - settings_updated() can dynamically resize

2. **Just configuration, not code**
   - Change settings value
   - Or add auto-detection helper

3. **No structural changes needed**
   - Phase 1.2 did all the work
   - This is just "turn the knob"

**Benefits:**
- **NVMe:** +100-150% throughput (500 MB/s → 1+ GB/s)
- **Saturate 10Gbps network:** Achieve full 1.25 GB/s
- **HDD:** No negative impact (can use lower thread count)
- **Configurable:** Adjust per deployment

**Testing:**
1. Benchmark with different thread counts: 2, 8, 16, 32
2. Measure NVMe write throughput
3. Measure network utilization
4. Verify HDD performance with low thread count

**Success Criteria:**
- ✅ NVMe write: 500 MB/s → 1+ GB/s (with 32 threads)
- ✅ Network utilization: 80-100%
- ✅ HDD: No regression (with 2-4 threads)
- ✅ Configurable via settings_pack

**Effort:** 1 day (mostly testing and documentation)

**Documentation:** Update README.md with recommended settings

**Dependencies:** Phase 1.2 (settings infrastructure)

---

### 📋 Phase 3: Unified Cache + Write Coalescing

**Goal:** Replace store_buffer with persistent cache and implement write coalescing

**Priority:** MEDIUM-HIGH (major performance gain for HDD, nice for SSD)

**Estimated Duration:** 4-6 days total

**Why Together:**
- Unified cache naturally supports write coalescing
- No need for separate `pending_writes` structure
- Cache tracks dirty entries, decides when to flush
- Clean architectural design

---

#### Phase 3.1: Unified Cache with Sharding

**What:** Replace `store_buffer` with persistent sharded cache

**Why After Phase 2:**
- ✅ Need Phase 1+2 as foundation
- ✅ Write coalescing needs persistent cache
- ✅ store_buffer is temporary (deleted after write)

**Key Difference from store_buffer:**

```cpp
// store_buffer (current - temporary)
async_write() {
    store_buffer.insert(location, buffer);
    post([=]() {
        pwrite(...);
        store_buffer.erase(location);  // ← Deleted immediately!
    });
}

// unified_cache (new - persistent)
async_write() {
    unified_cache.insert(location, buffer, dirty=true);
    // Cache keeps it until eviction or explicit flush
    // Can be read by async_read() even after write completes
}
```

**Design:** Sharded cache (32 partitions)

```cpp
class unified_cache {
    static constexpr size_t NUM_PARTITIONS = 32;
    std::array<cache_partition, NUM_PARTITIONS> partitions_;

    size_t get_partition(piece_index_t piece) {
        return static_cast<size_t>(piece) % NUM_PARTITIONS;
    }
};

class cache_partition {
    std::mutex mutex_;                    // Per-partition lock
    std::unordered_map<location, cache_entry> entries_;
    std::list<location> lru_list_;
    size_t max_entries_;
};

struct cache_entry {
    char* buffer;                         // 16KiB, malloc by cache
    bool dirty;                           // Needs writeback?
    time_point insert_time;               // For coalescing timeout
    std::function<void(storage_error)> handler;  // Completion callback
    std::list<location>::iterator lru_iter;
};
```

**Benefits:**
- ✅ 32x less mutex contention (vs single mutex)
- ✅ Persistent cache (improves read hit rate)
- ✅ Natural support for write coalescing
- ✅ Dirty tracking built-in

**When to Consider:**
- After Phase 1+2 complete
- Measured and want more performance
- Profiling shows benefit

**Effort:** 2-3 days

**Dependencies:** Phase 1 + 2 complete

---

#### Phase 3.2: Write Coalescing (Based on Unified Cache)

**What:** Accumulate dirty entries and flush with pwritev()

**Why After 3.1:**
- ✅ Requires unified_cache infrastructure
- ✅ Cache tracks dirty entries
- ✅ No separate `pending_writes` needed

**Design:**

```cpp
class cache_partition {
    // Track dirty entries per storage
    std::map<storage_index_t, std::set<location>> dirty_entries_;

    // Flush logic
    void check_flush_conditions(storage_index_t storage) {
        auto& dirty = dirty_entries_[storage];

        // Condition 1: Accumulated enough (64 blocks = 1MB)
        if (dirty.size() >= 64) {
            flush_dirty_entries(storage);
        }

        // Condition 2: Timeout (oldest entry > 100ms for HDD)
        auto oldest = find_oldest_dirty(storage);
        if (now() - oldest->insert_time > 100ms) {
            flush_dirty_entries(storage);
        }
    }

    void flush_dirty_entries(storage_index_t storage) {
        // 1. Collect dirty entries
        auto& dirty = dirty_entries_[storage];
        std::vector<cache_entry*> to_flush;
        for (auto& loc : dirty) {
            to_flush.push_back(&entries_[loc]);
        }

        // 2. Sort by (piece, offset)
        std::sort(to_flush.begin(), to_flush.end(),
            [](auto* a, auto* b) {
                return std::tie(a->location.piece, a->location.offset)
                     < std::tie(b->location.piece, b->location.offset);
            });

        // 3. Group contiguous blocks
        auto groups = group_contiguous(to_flush);

        // 4. Write each group with pwritev()
        for (auto& group : groups) {
            if (group.size() >= 4) {  // Worth coalescing
                partition_storage->writev(group);  // Batched write
            } else {
                for (auto* entry : group) {
                    partition_storage->write(entry);  // Individual write
                }
            }
        }

        // 5. Mark as clean (keep in cache!)
        for (auto* entry : to_flush) {
            entry->dirty = false;
        }
        dirty_entries_[storage].clear();
    }
};
```

**partition_storage::writev() implementation:**

```cpp
void partition_storage::writev(std::vector<cache_entry*> const& entries) {
    // Map to file_slices
    std::vector<slice_info> slices;
    for (auto* entry : entries) {
        auto fs = fs_.map_block(entry->location.piece,
                                 entry->location.offset,
                                 DEFAULT_BLOCK_SIZE);
        for (auto const& f : fs) {
            slices.push_back({
                .disk_offset = parse_offset(f.file_path),
                .buffer = entry->buffer + f.file_offset,
                .size = f.size
            });
        }
    }

    // Sort by disk offset
    std::sort(slices.begin(), slices.end(),
        [](auto& a, auto& b) { return a.disk_offset < b.disk_offset; });

    // Group contiguous slices
    auto groups = group_contiguous_slices(slices);

    // pwritev() each group
    for (auto& group : groups) {
        std::vector<iovec> iov;
        for (auto& slice : group) {
            iov.push_back({slice.buffer, slice.size});
        }
        pwritev(fd_, iov.data(), iov.size(), group[0].disk_offset);
    }
}
```

**Benefits:**
- **HDD:** +93% throughput (reduce seeks 64 → 1)
- **SSD:** +30-40% throughput (reduce syscalls)
- **No memory overhead:** Reuses cache entries
- **Clean design:** No separate pending_writes

**Effort:** 1-2 days (simpler than expected!)

**Dependencies:** Phase 3.1 (unified cache)

---

### 📋 Phase 4: io_uring (Optional)

**Goal:** Further reduce syscall overhead with io_uring

**Priority:** LOW (only if Phase 3 still insufficient)

**Estimated Duration:** 3-5 days

**What:** Use io_uring for async I/O instead of thread pool + pwritev/pwritev

**Why Optional:**
- ✅ Linux 5.1+ only (not portable)
- ✅ Complex error handling
- ⚠️ Additional dependency (liburing)
- ❓ Benefit: +20-30% on top of Phase 3 (if still needed)

**When to Consider:**
- After Phase 3 complete and measured
- If still not saturating network
- If syscall overhead still visible in profiling
- If targeting only modern Linux systems

**Conditions:**
- Don't use O_DIRECT (alignment complexity)
- Keep buffered I/O
- Only reduce syscall overhead
- Phase 3's write coalescing already reduces syscalls significantly

**Effort:** 3-5 days

**Dependencies:** Phase 3 complete

---

## Summary Table

| Phase | Description | Duration | Priority | Dependencies | Benefit |
|-------|-------------|----------|----------|--------------|---------|
| ✅ 0.1 | spdlog runtime control | ✅ Done | HIGH | None | Debug efficiency |
| ✅ 0.2 | Event-driven alerts | ✅ Done | HIGH | None | 5000x faster alerts |
| 🔄 1.1 | Buffer pool merger | 1-2 days | HIGH | None | +48% memory |
| 🔄 1.2 | Configurable cache + thread pools | 1 day | HIGH | 1.1 | Production ready |
| 🔄 2 | Parallel writes (config only) | 1 day | HIGH | 1.2 | NVMe +100-150% |
| ⏸️ 3.1 | Unified cache (sharded) | 2-3 days | MEDIUM | 1, 2 | Persistent cache |
| ⏸️ 3.2 | Write coalescing | 1-2 days | MEDIUM | 3.1 | HDD +93%, SSD +30% |
| ⏸️ 4 | io_uring | 3-5 days | LOW | 3 | +20-30% additional |

**Total Estimated Time:**
- **Phase 1 (Required):** 2-3 days
- **Phase 2 (Required):** 1 day
- **Phase 3 (Recommended):** 3-5 days
- **Phase 4 (Optional):** 3-5 days (measure Phase 3 first)
- **Total (Phase 1+2):** 3-4 days
- **Total (Phase 1+2+3):** 6-9 days

---

## Implementation Order

**Recommended Sequence:**

1. **Week 1: Foundation (Required)**
   - Day 1-2: Phase 1.1 (buffer pool merger)
   - Day 3: Phase 1.2 (configurable cache + thread pools from settings)
   - Day 4: Phase 2 (test with different thread counts, document optimal settings)
   - Day 5: Integration testing, measure baseline performance

2. **Week 2: Cache + Coalescing (Recommended for Production)**
   - Day 1-3: Phase 3.1 (unified cache implementation)
   - Day 4-5: Phase 3.2 (write coalescing based on cache)
   - Testing: HDD/SSD/NVMe benchmarks

3. **After Week 2:**
   - Measure results in production
   - Decide if Phase 4 (io_uring) needed based on profiling
   - If network still not saturated, consider Phase 4

---

## Success Metrics

### Phase 1 (Foundation)
- ✅ Memory efficiency: +40-48% for unbalanced workloads
- ✅ Configuration working: cache size adjustable, thread pools from settings
- ✅ Code quality: follows libtorrent conventions (m_ prefix)
- ✅ No regressions: all tests pass

### Phase 2 (Parallel Writes)
- ✅ NVMe: Throughput +100-150% (500 MB/s → 1+ GB/s)
- ✅ Network utilization: 80-100% (saturate 10Gbps)
- ✅ HDD: No regression with low thread count
- ✅ Configuration documented

### Phase 3 (Cache + Coalescing)
- ✅ Cache hit rate: >50% for typical workloads
- ✅ HDD: Write latency -60% or better (seek reduction)
- ✅ SSD: Throughput +30-40% (syscall reduction)
- ✅ Mutex contention: <5% (sharded design)
- ✅ Memory: No leaks, predictable usage

### Phase 4 (Optional)
- Decide after measuring Phase 3 results

---

## Testing Strategy

### Per-Phase Testing
1. **Unit tests:** Test individual components in isolation
2. **Integration tests:** Full read/write cycles
3. **Performance tests:** Measure latency/throughput
4. **Stress tests:** Saturate network, verify stability

### Full Regression Testing
After each phase:
- Run on HDD, SATA SSD, NVMe
- Balanced, read-heavy, write-heavy workloads
- Single client, 10 clients, 50 clients
- 1 hour stress test at full network speed

---

## Rollback Plan

Each phase is independent:
- Phase 1.1: Revert to split pools if issues
- Phase 1.2: Disable settings propagation
- Phase 2.1: Disable coalescing (flush immediately)
- Phase 2.2: Reduce thread count back to 8

All changes should be feature-flaggable for production safety.

---

## Risk Assessment

### Phase 1 (LOW RISK)
- Proven design (libtorrent 2.x)
- Short critical sections
- Extensive documentation

### Phase 2 (MEDIUM RISK)
- More complex (pending writes, backpressure)
- Careful testing needed
- Well-documented design

### Phase 3 (HIGH RISK)
- Optional - only if needed
- Complex implementations
- Consider effort vs. benefit

---

## References

**Design Documents:**
- `docs/BUFFER_POOL_MERGER.md` - Phase 1.1
- `docs/CACHE_SIZE_CONFIG.md` - Phase 1.2
- `docs/WRITE_COALESCING_DESIGN.md` - Phase 2.1 & 2.2
- `docs/STORE_BUFFER_WATERMARK.md` - Phase 2.1 backpressure
- `docs/UNIFIED_CACHE_DESIGN.md` - Phase 3.1
- `docs/MUTEX_ANALYSIS.md` - Proves single mutex safe
- `docs/SESSION_MEMORY.md` - Complete analysis history

**Code References:**
- `tmp/libtorrent-2.0.10/` - Reference implementation

---

**Document Version:** 2.0
**Last Updated:** 2025-12-14
**Status:** Ready for User Approval

---

## Next Steps

1. **User review:** Review this reformulated plan
2. **Approve/modify:** Confirm phase ordering and priorities
3. **Begin Phase 1.1:** Start with buffer pool merger
4. **Iterative progress:** Complete one phase at a time, test thoroughly

