# EZIO Design Review - Unified Cache

**Date:** 2024-12-14
**Status:** Ready for Implementation

This document reviews the unified cache design decisions.

**Note:** Version management and GitHub Actions auto-release have been merged to master branch.

---

## 1. Thread Pool Configuration

### Problem
- Thread pool sizes hardcoded to 8
- Cannot adjust for different hardware

### Solution: Read from settings_pack

**Current Implementation (raw_disk_io.cpp:125-127):**
```cpp
read_thread_pool_(8),   // ❌ Hardcoded
write_thread_pool_(8),
hash_thread_pool_(8)
```

**Proposed Change:**
```cpp
// raw_disk_io_constructor already has settings_interface!
std::unique_ptr<disk_interface> raw_disk_io_constructor(
    io_context &ioc,
    settings_interface const &sett,  // ← Can read aio_threads here
    counters &c)
{
    int aio_threads = sett.get_int(lt::settings_pack::aio_threads);
    return std::make_unique<raw_disk_io>(ioc, aio_threads);
}

// raw_disk_io constructor
raw_disk_io::raw_disk_io(io_context &ioc, int aio_threads) :
    ioc_(ioc),
    read_buffer_pool_(ioc),
    write_buffer_pool_(ioc),
    read_thread_pool_(aio_threads),   // ✅ Configurable
    write_thread_pool_(aio_threads),
    hash_thread_pool_(aio_threads)
{
}
```

**Benefits:**
- ✅ No ABI break (constructor already has settings_interface)
- ✅ Configurable via settings_pack
- ✅ Can auto-detect: `std::thread::hardware_concurrency()`

**Related:** Cache partition count will use `aio_threads * 2` or `* 4`

---

## 2. Unified Cache Design (CORE FEATURE)

### Background

**Previous Problem:**
- Implemented LRU cache with single global mutex
- **Severe performance issue**: Mutex contention dominated performance
- 8+ I/O threads competing for same lock
- Profiling confirmed: mutex is bottleneck, not cache logic

**Current store_buffer Problem:**
- Temporary only (deleted after write completes)
- No persistent cache
- async_read can only hit "currently being written" blocks

### Design Goal

Replace store_buffer with persistent, high-performance, low-contention cache.

### Architecture: Sharded Cache (Direction A)

```
┌────────────────────────────────────────────────────┐
│ buffer_pool (I/O temporary)                        │
│ - 128MB (~8192 entries)                            │
│ - Short-lived buffers for libtorrent interface    │
│ - Independent from cache                           │
└────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────┐
│ unified_cache (Persistent cache)                   │
│                                                     │
│  32 Partitions (hardcoded → configurable)         │
│  ┌──────┐ ┌──────┐ ┌──────┐       ┌──────┐       │
│  │ P0   │ │ P1   │ │ P2   │  ...  │ P31  │       │
│  │mutex │ │mutex │ │mutex │       │mutex │       │
│  └──────┘ └──────┘ └──────┘       └──────┘       │
│                                                     │
│  - 512MB (~32768 entries, configurable)           │
│  - Each entry: 16KiB fixed size                   │
│  - Cache malloc/free its own buffers              │
│  - partition = piece % num_partitions             │
│  - LRU eviction per partition                     │
└────────────────────────────────────────────────────┘
```

### Key Design Decisions

#### 4.1 Entry Count (Not Bytes)

**Design:**
```cpp
class unified_cache {
    size_t max_entries_;  // 32768 for 512MB

    unified_cache(size_t max_entries) : max_entries_(max_entries) {}

    void set_max_entries(size_t new_max) {
        // Dynamic resize
        max_entries_ = new_max;
        // Evict if shrinking
    }
};
```

**Why:**
- ✅ Clear: `32768 entries = 512MB` (32768 * 16KiB)
- ✅ Simple math: No byte tracking per entry
- ✅ Fixed 16KiB matches libtorrent block size
- ✅ Easy configuration: `cache_entries = (cache_mb * 1024) / 16`

**Examples:**
- 512MB = 32768 entries
- 1GB = 65536 entries
- 256MB = 16384 entries

#### 4.2 Cache Manages Own Buffers (Method A)

**CHOSEN: Method A**

```cpp
struct cache_entry {
    location loc;              // (torrent, piece, offset)
    char* buffer;              // malloc'ed by cache
    bool dirty;
    std::list<location>::iterator lru_iter;
};

class cache_partition {
    bool insert(location loc, char const* data) {
        // Cache malloc's its own buffer
        char* buf = static_cast<char*>(malloc(16384));
        if (!buf) return false;

        memcpy(buf, data, 16384);
        entries_[loc] = {loc, buf, true, ...};
        return true;
    }

    bool evict_one_lru() {
        // Cache free's its own buffer
        free(it->second.buffer);
        entries_.erase(it);
        return true;
    }
};
```

**Why Method A:**
- ✅ **Clear separation**: buffer_pool (I/O temp) vs cache (persistent)
- ✅ **Independent sizing**: Cache 512MB, I/O pool 128MB
- ✅ **No competition**: Don't fight for same buffer pool
- ✅ **Simple ownership**: No reference counting needed
- ✅ **Predictable memory**: Total = I/O + Cache

**REJECTED: Method B (buffer_pool dependency)**

Reasons for rejection:
- ❌ **Zero copy impossible**: Ownership conflict between cache and libtorrent
  - Cache needs: Long-term ownership (re-read multiple times)
  - Libtorrent needs: Exclusive ownership (frees when done)
  - Cannot resolve without complex reference counting
- ❌ **Negligible benefit**: Saving 1 memcpy (16KiB ~50ns) vs disk I/O (5-10ms)
- ❌ **Coupling**: Unclear responsibility between cache and buffer_pool

**Memory copy analysis:**
```
async_read cache hit:
- Method A: 1-2 memcpy (cache → temp → pool_buf) ~100ns
- Method B: 1 memcpy (cache → pool_buf) ~50ns
- Disk I/O: 5,000,000ns (HDD) or 100,000ns (SSD)
- Conclusion: Copy overhead < 0.002% of disk I/O, negligible
```

#### 4.3 Partitioning Strategy

**Phase 1.1: Hardcoded 32 partitions**
```cpp
static constexpr size_t NUM_PARTITIONS = 32;
std::array<cache_partition, NUM_PARTITIONS> partitions_;
```

**Phase 1.2: Configurable based on aio_threads**
```cpp
std::vector<cache_partition> partitions_;

unified_cache(size_t max_entries, int aio_threads) {
    // Formula: 2-4x thread count, min 16, power of 2
    size_t num = std::max(16, next_power_of_2(aio_threads * 2));
    partitions_.resize(num);

    size_t per_partition = max_entries / num;
    for (auto& p : partitions_) {
        p = cache_partition(per_partition);
    }
}
```

**NOT doing: Runtime dynamic resize**
- Would require rehashing all entries
- Would need to pause all I/O
- Complexity >> benefit
- Set once at startup is sufficient

**Partition selection:**
```cpp
size_t get_partition_index(location const& loc) const {
    return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
}
```

**Why `piece % num_partitions`:**
- ✅ **Perfect uniform distribution**: Pieces are sequential 0,1,2,...
- ✅ **Same-piece locality**: All blocks of piece N → partition N % 32
- ✅ **Simple and fast**: Single modulo operation
- ✅ **Predictable**: No hash function surprises

**Collision analysis:**
- 8 threads, 32 partitions: 8/32 = 25% max collision probability
- In practice much lower due to piece distribution over time
- Reduces from 100% (single mutex) to ~3% (32 mutexes)

#### 4.4 Hash Function Considerations

**Current hash (store_buffer.hpp:37-44):**
```cpp
std::size_t hash = 0;
boost::hash_combine(hash, l.torrent);  // Usually 0 or 1
boost::hash_combine(hash, l.piece);    // 0, 1, 2, 3, ...
boost::hash_combine(hash, l.offset);   // 0, 16384, 32768, ... (multiples of 2^14)
```

**Potential issue:**
- Offset is always multiple of 16384 (2^14)
- May cause poor distribution in unordered_map internal buckets

**Decision:**
- **Phase 1**: Use current boost::hash_combine
- **Profile first**: Check actual bucket distribution
- **Optimize later if needed**: FNV-1a hash or robin hood hashing

**Why wait:**
- Modern unordered_map implementations are robust
- Avoid premature optimization
- Profile-guided optimization better than speculation

#### 4.5 Eviction Policy

**Phase 1: LRU (Simple, Proven)**

```cpp
class cache_partition {
    std::list<location> lru_list_;  // MRU at front, LRU at back

    bool get(location loc, char* out) {
        auto it = entries_.find(loc);
        if (it != entries_.end()) {
            // Hit: move to front
            lru_list_.erase(it->second.lru_iter);
            lru_list_.push_front(loc);
            it->second.lru_iter = lru_list_.begin();

            memcpy(out, it->second.buffer, 16384);
            return true;
        }
        return false;  // Miss
    }

    bool evict_one_lru() {
        if (lru_list_.empty()) return false;

        // Evict from back (least recently used)
        location victim = lru_list_.back();
        lru_list_.pop_back();

        auto it = entries_.find(victim);
        if (it != entries_.end()) {
            if (it->second.dirty) {
                spdlog::warn("Evicting dirty entry: piece={}",
                             static_cast<int>(victim.piece));
                // TODO: Force writeback
            }
            free(it->second.buffer);
            entries_.erase(it);
            return true;
        }
        return false;
    }
};
```

**Advantages:**
- ✅ Simple implementation (~50 lines)
- ✅ O(1) access, eviction, promotion
- ✅ Well understood, easy to debug
- ✅ Sufficient for many workloads

**Backup: ARC (Adaptive Replacement Cache)**

**When to use:** If profiling shows LRU insufficient

**Why ARC for BitTorrent:**
- Distinguishes "recently used once" (T1) vs "frequently used" (T2)
- BitTorrent pattern: Sequential writes (T1) + Random verification (T2)
- Self-tuning, adapts to workload automatically
- 10-30% better hit rate in mixed workloads

**ARC Structure:**
```cpp
class cache_partition_arc {
    std::list<location> t1_;   // Recent (used once)
    std::list<location> t2_;   // Frequent (used multiple times)
    std::list<location> b1_;   // Ghost: recently evicted from T1
    std::list<location> b2_;   // Ghost: recently evicted from T2

    size_t p_;                 // Target size for T1 (self-tuning)

    // Adaptive algorithm adjusts p_ based on hit patterns
};
```

**Implementation strategy:**
1. Phase 1.1: Implement LRU first
2. Profile: Measure cache hit rate, eviction patterns
3. Decision point: If LRU hit rate < 60% or scan problems
4. Phase 1.2: Implement ARC as drop-in replacement

**Metrics to decide LRU vs ARC:**
- Cache hit rate < 60%: Consider ARC
- Too many "hot" entries evicted: ARC likely better
- Heavy verification/re-read workload: ARC advantage
- Simplicity preference: Keep LRU if good enough

**Complete ARC implementation in:** docs/UNIFIED_CACHE_DESIGN.md lines 507-603

#### 4.6 Dynamic Resize

```cpp
void unified_cache::set_max_entries(size_t new_max) {
    max_entries_ = new_max;

    // Update each partition
    size_t per_partition = new_max / partitions_.size();
    for (auto& p : partitions_) {
        p.set_max_entries(per_partition);
    }

    spdlog::info("Cache resized: {} entries ({} MB)",
                 new_max, (new_max * 16) / 1024);
}

void cache_partition::set_max_entries(size_t new_max) {
    std::lock_guard lock(mutex_);
    max_entries_ = new_max;

    // If shrinking, evict immediately
    while (entries_.size() > max_entries_) {
        evict_one_lru();
    }
}
```

**Called from:**
```cpp
void raw_disk_io::settings_updated() override {
    // cache_size unit: KiB (libtorrent convention)
    int cache_kb = m_settings->get_int(settings_pack::cache_size);
    size_t entries = (cache_kb * 1024) / 16384;

    m_cache.set_max_entries(entries);
}
```

#### 4.7 Integration with raw_disk_io

**Constructor:**
```cpp
raw_disk_io::raw_disk_io(io_context& ioc,
                         settings_interface const& sett,
                         counters& c)
    : ioc_(ioc),
      m_buffer_pool(ioc),                              // I/O temp (128MB)
      read_thread_pool_(get_aio_threads(sett)),
      write_thread_pool_(get_aio_threads(sett)),
      hash_thread_pool_(get_aio_threads(sett)),
      m_cache(calculate_cache_entries(sett)),         // Separate (512MB)
      m_settings(&sett)
{
}

static size_t calculate_cache_entries(settings_interface const& sett) {
    int cache_kb = sett.get_int(settings_pack::cache_size);
    return (cache_kb * 1024) / 16384;  // 16KiB per entry
}

static int get_aio_threads(settings_interface const& sett) {
    return sett.get_int(settings_pack::aio_threads);
}
```

**async_write flow:**
```cpp
bool raw_disk_io::async_write(..., char const *buf, ...) {
    // 1. Copy to cache (cache malloc's own buffer)
    m_cache.insert_write({storage, r.piece, r.start}, buf);

    // 2. Async write to disk
    boost::asio::post(write_thread_pool_, [=, this]() {
        storages_[storage]->write(buf, r.piece, r.start, r.length, error);

        // 3. Mark clean but KEEP in cache
        m_cache.mark_clean({storage, r.piece, r.start});

        handler(error);
    });

    return exceeded;
}
```

**async_read flow:**
```cpp
void raw_disk_io::async_read(...) {
    char temp_buf[16384];

    // 1. Try cache first (including dirty entries)
    if (m_cache.try_read({idx, r.piece, r.start}, temp_buf)) {
        // Cache hit! Copy to buffer_pool buffer
        char* pool_buf = m_buffer_pool.allocate_buffer();
        memcpy(pool_buf, temp_buf, 16384);
        handler(disk_buffer_holder(m_buffer_pool, pool_buf), error);
        return;
    }

    // 2. Cache miss: read from disk
    boost::asio::post(read_thread_pool_, [=, this]() {
        char disk_buf[16384];
        storages_[idx]->read(disk_buf, r.piece, r.start, r.length, error);

        // 3. Insert into cache for future reads
        m_cache.insert_write({idx, r.piece, r.start}, disk_buf);

        char* pool_buf = m_buffer_pool.allocate_buffer();
        memcpy(pool_buf, disk_buf, 16384);
        handler(disk_buffer_holder(m_buffer_pool, pool_buf), error);
    });
}
```

**Replaces store_buffer:**
```cpp
// OLD: store_buffer (raw_disk_io.cpp:272, 280)
store_buffer_.insert({storage, r.piece, r.start}, buffer.data());
// ... write ...
store_buffer_.erase({storage, r.piece, r.start});  // ❌ Deleted!

// NEW: unified_cache
m_cache.insert_write({storage, r.piece, r.start}, buffer.data());
// ... write ...
m_cache.mark_clean({storage, r.piece, r.start});   // ✅ Kept!
```

### Comparison with libtorrent PR #7013

**PR #7013 issues:**
- ❌ No read cache (major performance problem, 10x more I/O ops)
- ❌ Only write buffer (deleted after write)
- ❌ Mutex contention issues
- ❌ Cache stall problems

**Our design advantages:**
- ✅ Unified read/write cache
- ✅ Sharded for concurrency (32x less contention)
- ✅ Persistent entries (not just pending writes)
- ✅ Configurable size and partitions
- ✅ Independent memory management

### Performance Expectations

**Mutex contention:**
- Before: 1 mutex, 100% contention with 8 threads
- After: 32 mutexes, ~3% contention
- Lock duration: ~100ns (hash lookup + memcpy)

**Cache hit benefits:**
- Read: Avoid disk read (5-10ms HDD, 0.1ms SSD)
- Write: Serve recent writes immediately
- Verification: Pieces stay cached during hash check

**Memory overhead:**
- Cache: 512MB (32768 * 16KiB)
- Per-entry metadata: ~64 bytes
  - location: 16 bytes
  - buffer pointer: 8 bytes
  - dirty flag: 1 byte
  - lru_iter: 16 bytes
  - map overhead: ~24 bytes
- Total metadata: 32768 * 64 = 2MB (~0.4% overhead)

### Implementation Plan

**Phase 1.1: Core Implementation (2-3 days)**
1. Create cache_partition.hpp/.cpp
   - Basic structure with mutex, map, LRU list
   - insert(), get(), mark_clean(), evict_one_lru()
   - set_max_entries() for dynamic resize

2. Create unified_cache.hpp/.cpp
   - 32 partitions (hardcoded)
   - insert_write(), try_read(), mark_clean()
   - set_max_entries() dispatches to partitions
   - Statistics: total_entries(), total_size_mb()

3. Integrate with raw_disk_io
   - Add m_cache member
   - Modify async_write to use cache
   - Modify async_read to check cache first
   - Remove store_buffer code

4. Configuration
   - Read cache_size from settings_pack
   - Implement settings_updated()
   - Read aio_threads for thread pool sizing

5. Testing
   - Unit tests for cache_partition (LRU, eviction)
   - Integration test: write → read should hit cache
   - Performance test: measure contention reduction
   - Benchmark: throughput vs current store_buffer

**Phase 1.2: Optimizations (if needed)**
- Make partition count configurable (aio_threads * 2)
- Implement ARC if LRU hit rate insufficient
- Optimize hash function if distribution poor

---

## 3. Open Questions & Future Work

### 3.1 Dirty Entry Eviction

**Current design:**
- evict_one_lru() warns but doesn't writeback dirty entries
- Risk: Data loss if evicted before writeback

**Options:**
1. **Never evict dirty**: Only evict clean entries
   - Pro: No data loss risk
   - Con: Cache may fill with dirty entries

2. **Force writeback before evict**: Synchronous write
   - Pro: No data loss
   - Con: Eviction becomes slow (disk I/O)

3. **Async writeback + retry eviction**: Schedule writeback, try next entry
   - Pro: No blocking
   - Con: Complex, may still fill cache

**Decision:** Defer to Phase 1.1 implementation

### 3.2 Write Coalescing

**Opportunity:**
- Multiple writes to same location before flush
- Can coalesce in cache (just update existing entry)

**Current design:**
- insert() already updates existing entry
- Automatically gets write coalescing

**Future enhancement:**
- Track adjacent dirty blocks
- Flush multiple adjacent blocks with pwritev()
- This is Phase 2 (Write Optimization)

### 3.3 Cache Warmup

**Question:** Should cache be pre-populated?

**Options:**
1. Cold start (current design)
2. Pre-populate frequently accessed pieces
3. Restore cache from previous session

**Decision:** Cold start for Phase 1, revisit if needed

### 3.4 Cache Statistics

**Metrics to track:**
```cpp
struct cache_stats {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> dirty_evictions{0};

    double hit_rate() const {
        uint64_t total = hits + misses;
        return total > 0 ? (double)hits / total : 0.0;
    }
};
```

**Integration:**
- Per-partition stats
- Aggregate to global stats
- Log periodically (every 60s)
- Export via libtorrent counters

**Decision:** Implement in Phase 1.1

---

## 4. Files to Create/Modify

### Completed (Already in master)

1. ✅ **cmake/Version.cmake** - Version detection module
2. ✅ **version.txt** - git archive template
3. ✅ **.gitattributes** - export-subst directive
4. ✅ **.github/workflows/release.yml** - Auto-release workflow
5. ✅ **CMakeLists.txt** - Use cmake/Version.cmake, GIT_VERSION → EZIO_VERSION
6. ✅ **main.cpp** - GIT_VERSION → EZIO_VERSION
7. ✅ **config.cpp** - GIT_VERSION → EZIO_VERSION
8. ✅ **daemon.cpp** - GIT_VERSION → EZIO_VERSION
9. ✅ **service.cpp** - GIT_VERSION → EZIO_VERSION

### New Files (Phase 1.1 - To Implement)

1. **cache_partition.hpp** - Per-partition cache with LRU
2. **cache_partition.cpp** - Implementation
3. **unified_cache.hpp** - Sharded cache coordinator
4. **unified_cache.cpp** - Implementation

### Modified Files (Phase 1.1 - To Implement)

1. **raw_disk_io.hpp**
   - Add unified_cache member
   - Add settings_interface pointer
   - Change constructor signature (add aio_threads param)
   - Implement settings_updated()

2. **raw_disk_io.cpp**
   - Read aio_threads, cache_size from settings
   - Initialize thread pools with aio_threads
   - Integrate cache into async_read/async_write
   - Remove store_buffer usage
   - Implement settings_updated()

3. **CLAUDE.md**
   - ✅ Updated Phase 1.1 with unified cache design
   - ✅ Added memory separation explanation
   - ✅ Added ARC as backup plan

4. **docs/DESIGN_REVIEW.md** - This file

5. **docs/UNIFIED_CACHE_DESIGN.md** - ✅ Complete design document

### Files to Remove (Phase 1.1)

1. **store_buffer.hpp** - Replaced by unified_cache
2. **store_buffer.cpp** - If exists

---

## 5. Testing Strategy

### Unit Tests

**cache_partition tests:**
```cpp
TEST(CachePartition, BasicInsertGet) {
    cache_partition p(10);
    char data[16384] = "test";
    char out[16384];

    ASSERT_TRUE(p.insert({0, 0, 0}, data));
    ASSERT_TRUE(p.get({0, 0, 0}, out));
    ASSERT_EQ(memcmp(data, out, 16384), 0);
}

TEST(CachePartition, LRUEviction) {
    cache_partition p(2);  // Max 2 entries

    p.insert({0, 0, 0}, data1);
    p.insert({0, 1, 0}, data2);
    p.insert({0, 2, 0}, data3);  // Should evict {0,0,0}

    ASSERT_FALSE(p.get({0, 0, 0}, out));  // Evicted
    ASSERT_TRUE(p.get({0, 1, 0}, out));   // Still there
}

TEST(CachePartition, DynamicResize) {
    cache_partition p(10);

    // Fill with 10 entries
    for (int i = 0; i < 10; i++) {
        p.insert({0, i, 0}, data);
    }

    // Shrink to 5
    p.set_max_entries(5);
    ASSERT_EQ(p.size(), 5);  // 5 entries evicted
}
```

**unified_cache tests:**
```cpp
TEST(UnifiedCache, PartitionDistribution) {
    unified_cache cache(3200);  // 100 entries per partition

    // Insert 3200 entries (piece 0-3199)
    for (int i = 0; i < 3200; i++) {
        cache.insert_write({0, i, 0}, data);
    }

    // Check distribution (should be ~100 per partition)
    // Allow 20% variance
    ASSERT_NEAR(cache.total_entries(), 3200, 10);
}

TEST(UnifiedCache, WriteReadRoundtrip) {
    unified_cache cache(1000);

    char write_data[16384] = "test data";
    char read_data[16384];

    // Write
    cache.insert_write({0, 100, 0}, write_data);

    // Read
    ASSERT_TRUE(cache.try_read({0, 100, 0}, read_data));
    ASSERT_EQ(memcmp(write_data, read_data, 16384), 0);
}
```

### Integration Tests

```cpp
TEST(RawDiskIO, CacheHitOnRead) {
    // 1. Write a block
    async_write(..., data, ...);
    wait_for_completion();

    // 2. Read same block
    async_read(..., handler);

    // 3. Should hit cache (fast, no disk I/O)
    ASSERT_TRUE(read_from_cache);
    ASSERT_EQ(disk_read_count, 0);
}

TEST(RawDiskIO, DynamicCacheResize) {
    // 1. Set cache to 512MB
    settings.set_int(settings_pack::cache_size, 524288);
    dio.settings_updated();
    ASSERT_EQ(cache.max_entries(), 32768);

    // 2. Resize to 256MB
    settings.set_int(settings_pack::cache_size, 262144);
    dio.settings_updated();
    ASSERT_EQ(cache.max_entries(), 16384);
}
```

### Performance Tests

```cpp
TEST(Performance, MutexContention) {
    // Compare: single mutex vs 32 partitions

    // Test 1: 8 threads hammering single mutex LRU
    auto t1 = benchmark_lru_single_mutex(8_threads, 100000_ops);

    // Test 2: 8 threads with sharded cache
    auto t2 = benchmark_sharded_cache(8_threads, 100000_ops);

    // Expect: t2 much faster (less contention)
    ASSERT_LT(t2, t1 * 0.5);  // At least 2x faster
}

TEST(Performance, CacheHitRate) {
    // Realistic workload: sequential writes + verification reads

    for (int piece = 0; piece < 1000; piece++) {
        // Write 64 blocks
        for (int offset = 0; offset < 64 * 16384; offset += 16384) {
            async_write({0, piece, offset}, data);
        }

        // Verify (re-read)
        for (int offset = 0; offset < 64 * 16384; offset += 16384) {
            async_read({0, piece, offset});
        }
    }

    // Expect: High hit rate on verification reads
    ASSERT_GT(cache_hit_rate(), 0.8);  // > 80%
}
```

---

## 6. Migration Path

### Phase 0: Preparation (Completed)
- ✅ All design documents completed
- ✅ Version management system merged to master
- ✅ GitHub Actions configured and merged
- ✅ Architecture reviewed and approved

### Phase 1: Implementation

**Week 1-2: Core Cache (Phase 1.1)**
1. Implement cache_partition (1 day)
2. Implement unified_cache (1 day)
3. Unit tests (0.5 days)
4. Integrate with raw_disk_io (2 days)
5. Remove store_buffer (0.5 days)
6. Integration testing (1 day)
7. Performance testing (1 day)

**Week 3: Configuration (Phase 1.2)**
1. Implement settings_updated() (0.5 days)
2. Make partition count configurable (0.5 days)
3. Testing (0.5 days)

**Week 4: Buffer Pool Merger (Phase 1.3)**
1. Merge read_buffer_pool + write_buffer_pool (1 day)
2. Update size limits (0.5 days)
3. Testing (0.5 days)

### Phase 2: Write Optimization (Future)
- Write coalescing with pwritev()
- Parallel writes (increase thread pool)
- Backpressure system

---

## 7. Risk Assessment

### High Risk
None identified

### Medium Risk

1. **Cache memory overhead**
   - Risk: 512MB cache + 128MB I/O pool = 640MB total
   - Mitigation: Configurable, can reduce if needed
   - Monitoring: Track OOM events

2. **Dirty entry eviction**
   - Risk: Data loss if dirty entry evicted before writeback
   - Mitigation: Warn on dirty eviction, consider never-evict-dirty policy
   - Monitoring: Track dirty_eviction_count

### Low Risk

1. **Hash distribution**
   - Risk: Poor distribution in unordered_map buckets
   - Mitigation: Use boost::hash_combine initially, optimize if needed
   - Monitoring: Check bucket distribution in tests

2. **Partition count tuning**
   - Risk: 32 may not be optimal for all workloads
   - Mitigation: Make configurable based on aio_threads
   - Monitoring: Profile mutex contention

---

## 8. Success Criteria

### Phase 1.1 Success Criteria

1. **Functional:**
   - ✅ Cache hit on read after write
   - ✅ LRU eviction works correctly
   - ✅ Dynamic resize works (grow and shrink)
   - ✅ No memory leaks (valgrind clean)

2. **Performance:**
   - ✅ Mutex contention < 10% (down from ~90%)
   - ✅ Cache hit rate > 60% in typical workload
   - ✅ Throughput >= current store_buffer implementation

3. **Quality:**
   - ✅ All unit tests pass
   - ✅ All integration tests pass
   - ✅ No regressions in existing tests

### Phase 1.2 Success Criteria

1. **Configuration:**
   - ✅ settings_updated() works correctly
   - ✅ Cache resizes without issues
   - ✅ Thread pool sizes configurable from settings_pack

### Overall Success

**Primary goal:** Replace store_buffer with high-performance persistent cache
**Key metric:** Throughput maintained or improved with better cache hit rate
**Quality bar:** Production-ready, well-tested, documented

---

## 9. Conclusion

### Completed Work (Merged to master)
✅ **Version Management** - Automatic version in tarballs via git archive
✅ **GitHub Actions** - Auto-release workflow configured

### Design Ready for Implementation

All unified cache design decisions reviewed and documented:

✅ **Thread Pool Config** - Read from settings_pack (aio_threads)
✅ **Unified Cache** - Complete design with Method A (cache-owned buffers)
✅ **Partitioning** - 32 partitions (hardcode → configurable)
✅ **Eviction** - LRU (Phase 1), ARC backup plan documented
✅ **Memory Management** - Entry count based, predictable sizing
✅ **Integration** - Clear async_read/write flow, replaces store_buffer

**Ready for implementation: Phase 1.1 (Core Cache)**

**Estimated effort:** 2-3 days for core implementation + testing

**Priority:**
- Phase 0 (Logging) - Can start anytime for better debugging
- Phase 1.1 (Unified Cache) - Core feature, documented and ready
