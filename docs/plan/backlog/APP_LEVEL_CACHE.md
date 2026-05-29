# EZIO Application-Level Cache Design

**Version:** 2.0
**Date:** 2025-12-14
**Status:** Design document for Phase 1.1 implementation
**Related:** CLAUDE.md, UNIFIED_CACHE_DESIGN.md

---

## Table of Contents

1. [Problem Diagnosis](#problem-diagnosis)
2. [Design Requirements](#design-requirements)
3. [Architecture Overview](#architecture-overview)
4. [Implementation Strategy](#implementation-strategy)
5. [Integration with raw_disk_io](#integration-with-raw_disk_io)
6. [Performance Analysis](#performance-analysis)
7. [Configuration](#configuration)
8. [Future Optimizations](#future-optimizations)

---

## Problem Diagnosis

### Current Issue: Temporary Cache Only

**Location:** `raw_disk_io.cpp:282-286`

**Current Behavior:**
```cpp
// After writing to disk, immediately erase from cache
write_buffer_pool_.push_disk_buffer_holders(
    [=, this, buffer = std::move(buffer)]() mutable {
        store_buffer_.erase({storage, r.piece, r.start});  // ← Deleted!
        SPDLOG_INFO("erase disk buffer from store_buffer");
    }
);
```

### The Problem

**store_buffer is temporary, not persistent:**

```
Timeline of Problem:

T0: async_write(block A) called
    → store_buffer_.insert(A, buffer)     ✓ Data enters cache
    → Submit write to thread pool
    → Return to libtorrent

T1: libtorrent calls async_read(block A)
    → store_buffer_.get(A)                ✓ Cache hit (data still pending write)

T2: Write thread completes disk write
    → pwrite(block A) completes
    → store_buffer_.erase(A)              ✗ Cache entry deleted!

T3: libtorrent calls async_read(block A) again (hash verification or upload)
    → store_buffer_.get(A)                ✗ Cache MISS!
    → Must read from disk (HDD: 12ms, wasted!)
```

### Real-World Impact

**Scenario: Download and verify piece**

```
Download piece 5 (256KB = 16 blocks):
1. Receive 16 blocks, async_write for each        ✓
2. libtorrent calls async_hash(piece 5)
   → Needs to read all 16 blocks
   → Cache cleared after write
   → 16 disk reads × 12ms = 192ms on HDD          ✗
   → Should be ~0.01ms cache hit!

Result: 19,200x slower than necessary
```

### Root Cause

**EZIO needs persistent application-level cache:**

- libtorrent 1.x had built-in disk cache
- libtorrent 2.0 removed it, relies on OS page cache
- EZIO uses `pread`/`pwrite` which do benefit from OS cache
- **BUT**: Application-level cache needed for:
  1. Immediate access to just-written blocks (dirty data)
  2. Read cache for frequently accessed pieces (seeding)
  3. Hash verification without disk round-trip
  4. Control over eviction policy (know what to keep)

---

## Design Requirements

### Functional Requirements

1. **Persistent Cache**
   - Don't delete after disk write
   - Keep entries until evicted by capacity limit

2. **Dirty/Clean State Tracking**
   - **Dirty**: Not yet written to disk, cannot evict
   - **Clean**: Already on disk, can evict if needed

3. **Unified Read/Write Cache**
   - async_write inserts entries
   - async_read queries cache first (both dirty and clean)
   - Replaces temporary store_buffer with persistent cache

4. **Low Mutex Contention**
   - Previous LRU cache had severe contention with 8+ threads
   - Solution: Sharded cache (32 partitions)

5. **Configurable Size**
   - Default: 512MB
   - From settings_pack::cache_size

### Non-Functional Requirements

| Metric | Target |
|--------|--------|
| Cache hit latency | < 1μs |
| Cache miss overhead | < 10μs |
| Mutex contention | < 5% |
| Hit rate (download) | > 80% |
| Hit rate (seeding) | > 90% |

---

## Architecture Overview

### Single-Tier Sharded Cache

**Design Philosophy:**
- ✅ **Simple first**: Start with proven LRU eviction
- ✅ **Sharding for concurrency**: 32 partitions reduce contention 32x
- ✅ **Persistent entries**: Keep data after disk write
- ✅ **Profile before optimizing**: If LRU insufficient, consider ARC later

```
┌─────────────────────────────────────────────────────────┐
│                  unified_cache (512MB)                   │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────┐ ┌──────────┐      ┌──────────┐          │
│  │Partition │ │Partition │ ...  │Partition │          │
│  │    0     │ │    1     │      │   31     │          │
│  │  mutex   │ │  mutex   │      │  mutex   │          │
│  │   LRU    │ │   LRU    │      │   LRU    │          │
│  └──────────┘ └──────────┘      └──────────┘          │
│                                                          │
│  Partition assignment: piece % 32                        │
│  Per-partition capacity: total_entries / 32              │
│  Eviction: LRU within partition, never evict dirty       │
│                                                          │
└─────────────────────────────────────────────────────────┘
                         ↕
                  [Raw Disk I/O]
```

### Key Design Decisions

**1. Why 32 partitions?**
- With 8 I/O threads: collision rate ≈ 8/32 = 25% worst case
- In practice much lower (piece accesses spread over time)
- Reduces contention from 100% (single mutex) to ~3%
- All blocks of same piece in same partition (simplifies consistency)

**2. Why partition by `piece % 32`?**
- Pieces are sequential: 0, 1, 2, 3, ...
- Perfect uniform distribution
- Same-piece locality (all blocks of piece N in same partition)

**3. Why LRU (not ARC)?**
- **Phase 1**: Simple LRU (proven, 50 lines of code)
- **Backup plan**: If profiling shows LRU hit rate < 60%, consider ARC
- See UNIFIED_CACHE_DESIGN.md for ARC details

**4. Why cache manages its own buffers?**
- Separate from buffer_pool (temporary I/O buffers)
- Cache uses malloc/free for persistent 16KiB blocks
- Clear ownership: cache controls lifetime
- No competition with I/O pool

---

## Implementation Strategy

### Data Structures

```cpp
// cache_entry.hpp
struct cache_entry {
    enum class state { DIRTY, CLEAN, FLUSHING };

    torrent_location location;     // (torrent, piece, offset)
    char* data;                     // 16KiB, malloc'ed by cache
    std::atomic<state> state_;
    size_t data_size;               // Always 16384

    // LRU metadata
    std::list<torrent_location>::iterator lru_iter;

    cache_entry(torrent_location loc, char const* d, state s = state::DIRTY)
        : location(loc), data(new char[16384]), state_(s), data_size(16384) {
        std::memcpy(data, d, 16384);
    }

    ~cache_entry() {
        delete[] data;
    }
};
```

```cpp
// cache_partition.hpp
class cache_partition {
private:
    std::mutex m_mutex;
    std::unordered_map<torrent_location, cache_entry> m_entries;
    std::list<torrent_location> m_lru_list;  // MRU front, LRU back
    size_t m_max_entries;

    // Statistics
    std::atomic<uint64_t> m_hits{0};
    std::atomic<uint64_t> m_misses{0};
    std::atomic<uint64_t> m_evictions{0};

public:
    explicit cache_partition(size_t max_entries);

    // Cache operations
    bool insert(torrent_location const& loc, char const* data,
                cache_entry::state state = cache_entry::state::DIRTY);
    bool get(torrent_location const& loc, char* out);
    void mark_clean(torrent_location const& loc);

    // Dynamic resize
    void set_max_entries(size_t new_max);

    // Stats
    size_t size() const;
    uint64_t hits() const { return m_hits.load(); }
    uint64_t misses() const { return m_misses.load(); }

private:
    bool evict_one_lru();  // Returns false if all entries dirty
    void promote_to_mru(torrent_location const& loc);
};
```

```cpp
// unified_cache.hpp
class unified_cache {
private:
    static constexpr size_t NUM_PARTITIONS = 32;
    std::array<cache_partition, NUM_PARTITIONS> m_partitions;
    size_t m_max_entries;  // Total capacity

public:
    explicit unified_cache(size_t max_entries);

    // Resize from settings_updated()
    void set_max_entries(size_t new_max);

    // async_write: insert dirty entry
    bool insert_write(torrent_location const& loc, char const* data);

    // async_read: query cache
    bool try_read(torrent_location const& loc, char* out);

    // After disk write: mark clean but keep in cache
    void mark_clean(torrent_location const& loc);

    // Statistics
    struct stats {
        uint64_t hits;
        uint64_t misses;
        uint64_t evictions;
        uint64_t dirty_blocks;
        uint64_t clean_blocks;
        double hit_rate;
    };
    stats get_stats() const;

private:
    size_t get_partition_index(torrent_location const& loc) const {
        return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
    }
};
```

### Cache Operations

**Insert (async_write):**
```cpp
bool unified_cache::insert_write(torrent_location const& loc, char const* data) {
    size_t idx = get_partition_index(loc);
    return m_partitions[idx].insert(loc, data, cache_entry::state::DIRTY);
}

bool cache_partition::insert(torrent_location const& loc, char const* data,
                              cache_entry::state state) {
    std::lock_guard lock(m_mutex);

    // Check if already exists (update)
    auto it = m_entries.find(loc);
    if (it != m_entries.end()) {
        std::memcpy(it->second.data, data, 16384);
        it->second.state_.store(state);
        promote_to_mru(loc);
        return true;
    }

    // Need space for new entry
    while (m_entries.size() >= m_max_entries) {
        if (!evict_one_lru()) {
            return false;  // All entries dirty, cannot evict
        }
    }

    // Create new entry
    cache_entry entry(loc, data, state);
    m_lru_list.push_front(loc);
    entry.lru_iter = m_lru_list.begin();
    m_entries[loc] = std::move(entry);

    return true;
}
```

**Query (async_read):**
```cpp
bool unified_cache::try_read(torrent_location const& loc, char* out) {
    size_t idx = get_partition_index(loc);
    return m_partitions[idx].get(loc, out);
}

bool cache_partition::get(torrent_location const& loc, char* out) {
    std::lock_guard lock(m_mutex);

    auto it = m_entries.find(loc);
    if (it == m_entries.end()) {
        m_misses.fetch_add(1);
        return false;
    }

    m_hits.fetch_add(1);
    std::memcpy(out, it->second.data, 16384);
    promote_to_mru(loc);
    return true;
}
```

**Mark Clean (after disk write):**
```cpp
void unified_cache::mark_clean(torrent_location const& loc) {
    size_t idx = get_partition_index(loc);
    m_partitions[idx].mark_clean(loc);
}

void cache_partition::mark_clean(torrent_location const& loc) {
    std::lock_guard lock(m_mutex);

    auto it = m_entries.find(loc);
    if (it != m_entries.end()) {
        it->second.state_.store(cache_entry::state::CLEAN);
    }
}
```

**LRU Eviction:**
```cpp
bool cache_partition::evict_one_lru() {
    if (m_lru_list.empty()) return false;

    // Find first CLEAN entry from LRU end
    for (auto it = m_lru_list.rbegin(); it != m_lru_list.rend(); ++it) {
        auto entry_it = m_entries.find(*it);
        if (entry_it != m_entries.end() &&
            entry_it->second.state_.load() == cache_entry::state::CLEAN) {

            // Evict this entry
            m_lru_list.erase(entry_it->second.lru_iter);
            m_entries.erase(entry_it);
            m_evictions.fetch_add(1);
            return true;
        }
    }

    // All entries are DIRTY, cannot evict
    return false;
}

void cache_partition::promote_to_mru(torrent_location const& loc) {
    auto it = m_entries.find(loc);
    if (it != m_entries.end()) {
        m_lru_list.erase(it->second.lru_iter);
        m_lru_list.push_front(loc);
        it->second.lru_iter = m_lru_list.begin();
    }
}
```

---

## Integration with raw_disk_io

### Modifications to raw_disk_io

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
private:
    buffer_pool m_buffer_pool;      // Temporary I/O buffers (128MB)
    unified_cache m_cache;          // Persistent cache (512MB, separate)

    // Remove store_buffer (replaced by m_cache)
    // store_buffer store_buffer_;  ← DELETE THIS

public:
    raw_disk_io(io_context& ioc, settings_interface const& sett, counters& c);
    void settings_updated() override;
};
```

### async_write Flow

```cpp
bool raw_disk_io::async_write(storage_index_t storage, peer_request const& r,
                               char const* buf, std::shared_ptr<disk_observer> o,
                               std::function<void(storage_error const&)> handler,
                               disk_job_flags_t flags) {
    torrent_location loc{storage, r.piece, r.start};

    bool exceeded = false;
    char* buffer = m_buffer_pool.allocate_buffer(exceeded, o);

    if (buffer) {
        std::memcpy(buffer, buf, r.length);

        // ★ NEW: Insert to persistent cache (DIRTY state)
        m_cache.insert_write(loc, buffer);

        // Submit write task
        boost::asio::post(write_thread_pool_,
            [=, this, handler = std::move(handler)]() mutable {
                storage_error error;
                storages_[storage]->write(buffer, r.piece, r.start, r.length, error);

                // ★ NEW: Mark clean but KEEP in cache
                m_cache.mark_clean(loc);

                // Free I/O buffer (cache has its own copy)
                m_buffer_pool.free_disk_buffer(buffer);

                post(ioc_, [=, h = std::move(handler)] { h(error); });
            });

        return exceeded;
    }

    // Sync write fallback
    storage_error error;
    storages_[storage]->write(const_cast<char*>(buf), r.piece, r.start, r.length, error);
    m_cache.insert_write(loc, buf);  // Insert as CLEAN
    post(ioc_, [=, h = std::move(handler)] { h(error); });
    return exceeded;
}
```

### async_read Flow

```cpp
void raw_disk_io::async_read(storage_index_t idx, peer_request const& r,
                              std::function<void(disk_buffer_holder, storage_error const&)> handler,
                              disk_job_flags_t flags) {
    torrent_location loc{idx, r.piece, r.start};

    // ★ NEW: Check cache first (includes both DIRTY and CLEAN)
    char temp_buffer[DEFAULT_BLOCK_SIZE];
    if (m_cache.try_read(loc, temp_buffer)) {
        // Cache hit!
        char* buf = m_buffer_pool.allocate_buffer();
        if (buf) {
            std::memcpy(buf, temp_buffer, r.length);
            disk_buffer_holder holder(m_buffer_pool, buf, DEFAULT_BLOCK_SIZE);
            storage_error error;
            handler(std::move(holder), error);
            return;
        }
    }

    // Cache miss: read from disk
    char* buf = m_buffer_pool.allocate_buffer();
    if (!buf) {
        storage_error error;
        error.ec = libtorrent::errors::no_memory;
        handler(disk_buffer_holder{}, error);
        return;
    }

    disk_buffer_holder buffer(m_buffer_pool, buf, DEFAULT_BLOCK_SIZE);

    boost::asio::post(read_thread_pool_,
        [=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
            storage_error error;
            storages_[idx]->read(buf, r.piece, r.start, r.length, error);

            // ★ NEW: Insert to cache after successful read (CLEAN state)
            if (!error.ec) {
                m_cache.insert_write(loc, buf);
                m_cache.mark_clean(loc);
            }

            post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
                h(std::move(b), error);
            });
        });
}
```

---

## Performance Analysis

### Scenario 1: Download + Hash Verification

```
Current (no persistent cache):
T0: Download 16 blocks (piece 5)
    → 16 async_write, all succeed
T1: async_hash(piece 5)
    → 16 disk reads: 16 × 12ms = 192ms (HDD)

With unified_cache:
T0: Download 16 blocks
    → 16 async_write, all inserted to cache (DIRTY)
T1: async_hash(piece 5)
    → 16 cache hits: 16 × 1μs = 0.016ms

Speedup: 192ms / 0.016ms = 12,000x
```

### Scenario 2: Seeding (Repeated Reads)

```
Request 1: async_read(piece 100)
    → Cache miss: read from disk (12ms HDD)
    → Insert to cache (CLEAN)

Requests 2-100: async_read(piece 100)
    → Cache hit: 1μs each
    → Zero disk I/O

Speedup: 12ms / 1μs = 12,000x
```

### Expected Hit Rates

| Workload | Expected Hit Rate | Benefit |
|----------|-------------------|---------|
| Downloading | 80-90% | Avoid disk reads for hash verification |
| Seeding | 90-95% | Popular pieces stay in cache |
| Mixed | 85-90% | Best of both |

### Memory Usage

| Component | Size | Purpose |
|-----------|------|---------|
| buffer_pool | 128 MB | Temporary I/O buffers |
| unified_cache | 512 MB | Persistent cache (32768 entries × 16KiB) |
| **Total** | **640 MB** | **Predictable, controlled** |

Comparison:
- Current: store_buffer unbounded (risk of OOM)
- New: 640 MB fixed, configurable

---

## Configuration

### From settings_pack

```cpp
raw_disk_io::raw_disk_io(io_context& ioc, settings_interface const& sett, counters& c)
    : m_buffer_pool(ioc),
      m_cache(calculate_cache_entries(sett)),
      m_settings(&sett),
      m_stats_counters(c)
{
}

void raw_disk_io::settings_updated() {
    // Update cache size dynamically
    size_t new_entries = calculate_cache_entries(*m_settings);
    m_cache.set_max_entries(new_entries);

    // Update buffer pool
    m_buffer_pool.set_settings(*m_settings);
}

size_t raw_disk_io::calculate_cache_entries(settings_interface const& sett) {
    // cache_size is in KiB (libtorrent convention)
    int cache_kb = sett.get_int(settings_pack::cache_size);
    return (cache_kb * 1024) / 16384;  // 16KiB per entry
}
```

### Example Configurations

```cpp
// 512 MB cache (default)
cache_size = 512 * 1024;  // KiB
entries = (512 * 1024 * 1024) / 16384 = 32768

// 1 GB cache (high-memory system)
cache_size = 1024 * 1024;
entries = 65536

// 256 MB cache (low-memory system)
cache_size = 256 * 1024;
entries = 16384
```

---

## Future Optimizations

### Phase 2: Advanced Eviction (If Needed)

**Condition:** LRU hit rate < 60% in production

**Option: ARC (Adaptive Replacement Cache)**
- Distinguishes "recently used" (T1) vs "frequently used" (T2)
- Self-tuning based on workload
- Better for mixed sequential/random patterns
- See UNIFIED_CACHE_DESIGN.md for detailed ARC design

**Implementation:**
- Drop-in replacement for cache_partition
- Same interface, different eviction logic
- A/B test: measure hit rate improvement

### Phase 3: Lock-Free Optimization (If Needed)

**Condition:** Profiling shows mutex contention > 5%

**Option: Thread Affinity (Direction B1)**
- Each thread exclusively owns certain partitions
- No mutex needed (exclusive access)
- More complex, requires io_context per thread
- See UNIFIED_CACHE_DESIGN.md section "Direction B1"

---

## Summary

### Problems Solved

1. ✅ **Persistent cache**: Entries stay after disk write
2. ✅ **Low contention**: 32 partitions reduce mutex contention 32x
3. ✅ **Unified read/write**: Both operations benefit from cache
4. ✅ **Dirty tracking**: Never evict uncommitted data
5. ✅ **Configurable size**: Adjustable via settings_pack
6. ✅ **Simple design**: LRU proven, easy to maintain

### Performance Gains

| Metric | Current | With Cache | Improvement |
|--------|---------|------------|-------------|
| Hash verification (HDD) | 192ms | 0.016ms | **12,000x** |
| Repeated reads (HDD) | 12ms | 1μs | **12,000x** |
| Hit rate | N/A | 85-90% | N/A |
| Mutex contention | 100% | ~3% | **97% reduction** |

### Implementation Priority

**Phase 1.1: Unified Cache with LRU** (Current)
- Implement sharded cache (32 partitions)
- LRU eviction per partition
- Integrate with async_read/write
- Remove store_buffer
- **Effort:** 2-3 days

**Phase 1.2: Configuration** (After 1.1)
- Dynamic cache size from settings_pack
- Add statistics logging
- **Effort:** 1 day

**Phase 2: Advanced Optimizations** (Optional)
- Only if profiling shows need
- ARC eviction or lock-free design
- See UNIFIED_CACHE_DESIGN.md

---

## References

- **CLAUDE.md**: Project overview and optimization roadmap
- **UNIFIED_CACHE_DESIGN.md**: Complete technical design with ARC details
- **SESSION_MEMORY.md**: Analysis history and decision rationale
- **CACHE_BRANCH_ANALYSIS.md**: Post-mortem of previous cache attempt

---

**Document Version:** 2.0
**Status:** Ready for Phase 1.1 implementation
**Next Steps:** Implement cache_partition and unified_cache classes
