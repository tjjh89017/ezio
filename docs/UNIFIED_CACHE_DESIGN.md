# Unified Cache Design

## Background

### Current Problem

**Previous LRU Implementation Performance Issues:**
- Single global mutex causes severe contention with multiple I/O threads
- 8 read threads + 8 write threads competing for same lock
- Mutex contention dominates performance, not cache logic itself

**Current store_buffer Limitations:**
- Only caches blocks pending write to disk (raw_disk_io.cpp:272)
- Erases entry immediately after write completes (raw_disk_io.cpp:280)
- No persistent read cache
- async_read can only hit blocks "currently being written"

### Design Goals

1. **Reduce mutex contention** - Primary goal based on LRU profiling results
2. **Unified read/write cache** - Replace temporary store_buffer with persistent cache
3. **Configurable size** - Allow tuning based on available memory
4. **Simple, maintainable** - Avoid over-engineering (no complex multi-tier cache)

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│ buffer_pool (Memory Management)                      │
│ - alloc() / free()                                   │
│ - Manages disk_buffer allocation/recycling          │
│ - Does not care about data content                  │
└─────────────────────────────────────────────────────┘
                    ↑
                    │ Request/return buffers
                    ↓
┌─────────────────────────────────────────────────────┐
│ unified_cache (Sharded Cache)                       │
│                                                      │
│ ┌──────────┐ ┌──────────┐      ┌──────────┐       │
│ │Partition │ │Partition │ ...  │Partition │       │
│ │    0     │ │    1     │      │   31     │       │
│ │  mutex   │ │  mutex   │      │  mutex   │       │
│ └──────────┘ └──────────┘      └──────────┘       │
│                                                      │
│ - 32 partitions (hardcoded, may be configurable)   │
│ - partition = piece % 32                            │
│ - Each partition: independent mutex + hash map     │
│ - Cache entry: 16KiB fixed size                    │
│ - LRU eviction per partition                       │
└─────────────────────────────────────────────────────┘
                    ↓
            [Raw Disk I/O]
```

## Design Direction A: Sharded Cache with Mutex (Primary)

### Key Design Decisions

**1. Partitioning Strategy**

```cpp
constexpr size_t NUM_PARTITIONS = 32;

size_t get_partition_index(location const& loc) {
    return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
}
```

**Why `piece % 32`?**
- Pieces are sequential integers: 0, 1, 2, 3, ...
- Perfect uniform distribution guaranteed
- All blocks of same piece go to same partition (simplifies consistency)

**Why 32 partitions?**
- With 8 threads, probability of collision ≈ 8/32 = 25% worst case
- In practice much lower due to piece distribution across time
- Reduces contention from 100% (single mutex) to ~3% (32 mutexes)

**2. Cache Entry Structure**

```cpp
struct cache_entry {
    location loc;              // (torrent, piece, offset)
    char* buffer;              // 16KiB, malloc'ed by cache itself
    bool dirty;                // Needs writeback?

    // LRU metadata (per partition)
    std::list<location>::iterator lru_iter;
};
```

**Fixed 16KiB size:**
- Matches libtorrent block size exactly
- Simplifies memory accounting: `total_memory = num_entries * 16384`
- No need to track variable-sized entries
- **Cache manages its own buffers** (independent from buffer_pool)

**3. Per-Partition Structure**

```cpp
class cache_partition {
    std::mutex mutex_;
    std::unordered_map<location, cache_entry> entries_;
    std::list<location> lru_list_;  // MRU at front, LRU at back
    size_t max_entries_;                        // Entry count limit

public:
    cache_partition(size_t max_entries);

    // Cache operations
    bool insert(location loc, char const* data);   // Copy 16KiB data
    bool get(location loc, char* out);             // Copy 16KiB out
    void mark_clean(location loc);

    // Dynamic resize
    void set_max_entries(size_t new_max);          // Evicts if shrinking

    // Stats
    size_t size() const { return entries_.size(); }
    size_t max_entries() const { return max_entries_; }

private:
    bool evict_one_lru();  // Returns false if nothing to evict
};
```

**4. Unified Cache Interface**

```cpp
class unified_cache {
    static constexpr size_t NUM_PARTITIONS = 32;
    std::array<cache_partition, NUM_PARTITIONS> partitions_;
    size_t max_entries_;  // e.g., 32768 for 512MB

public:
    // Constructor: max_entries = total cache size / 16KiB
    // Example: 512MB = 32768 entries
    unified_cache(size_t max_entries);

    // Dynamic resize (from settings_updated)
    void set_max_entries(size_t new_max);

    // async_write: Copy data to cache, mark dirty
    bool insert_write(location loc, char const* data);

    // async_read: Check cache first
    bool try_read(location loc, char* out);

    // After disk write completes: clear dirty flag but keep entry
    void mark_clean(location loc);

    // Stats
    size_t total_entries() const;
    size_t max_entries() const { return max_entries_; }
    size_t total_size_mb() const { return (total_entries() * 16) / 1024; }

private:
    size_t get_partition_index(location const& loc) const {
        return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
    }
};
```

### Integration with raw_disk_io

**async_write Flow:**

```cpp
bool raw_disk_io::async_write(..., char const *buf, ...) {
    // 1. Copy to cache (mark dirty)
    cache_.insert_write({storage, r.piece, r.start}, buf);

    // 2. Async write to disk
    boost::asio::post(write_thread_pool_, [...]() {
        storages_[storage]->write(buf, ...);

        // 3. After write: mark clean (but keep in cache!)
        cache_.mark_clean({storage, r.piece, r.start});
    });
}
```

**async_read Flow:**

```cpp
void raw_disk_io::async_read(...) {
    // 1. Try cache first (including dirty entries)
    char buf[16384];
    if (cache_.try_read({idx, r.piece, r.start}, buf)) {
        handler(buffer, error);  // Cache hit!
        return;
    }

    // 2. Cache miss: read from disk
    boost::asio::post(read_thread_pool_, [...]() {
        storages_[idx]->read(buf, ...);

        // 3. Insert into cache for future reads
        cache_.insert_read({idx, r.piece, r.start}, buf);

        handler(buffer, error);
    });
}
```

### Memory Management Strategy

**Cache manages its own buffers (Method A - Chosen):**

```cpp
// cache_partition.cpp
bool cache_partition::insert(location loc, char const* data) {
    std::lock_guard lock(mutex_);

    // Check capacity, evict if needed
    while (entries_.size() >= max_entries_) {
        if (!evict_one_lru()) {
            return false;  // Cannot evict
        }
    }

    // Cache allocates its own 16KiB buffer (independent from buffer_pool)
    char* buf = static_cast<char*>(malloc(16384));
    if (!buf) {
        spdlog::error("Cache malloc failed!");
        return false;
    }

    memcpy(buf, data, 16384);

    cache_entry entry;
    entry.loc = loc;
    entry.buffer = buf;
    entry.dirty = true;

    lru_list_.push_front(loc);
    entry.lru_iter = lru_list_.begin();

    entries_[loc] = std::move(entry);
    return true;
}

bool cache_partition::evict_one_lru() {
    if (lru_list_.empty()) return false;

    location victim = lru_list_.back();
    lru_list_.pop_back();

    auto it = entries_.find(victim);
    if (it != entries_.end()) {
        // TODO: If dirty, need to write back to disk first
        if (it->second.dirty) {
            spdlog::warn("Evicting dirty cache entry: piece={}",
                         static_cast<int>(victim.piece));
        }

        free(it->second.buffer);  // Cache frees its own buffer
        entries_.erase(it);
        return true;
    }
    return false;
}
```

**Why separate from buffer_pool:**
- ✅ **Clear separation**: buffer_pool for temporary I/O, cache for persistent storage
- ✅ **Independent sizing**: Cache can be 512MB while I/O pool is 128MB
- ✅ **No competition**: I/O and cache don't fight for same buffer pool
- ✅ **Predictable memory**: Total = I/O pool + Cache (fixed, knowable)

### Configuration

```cpp
// From settings_pack: cache_size in KiB
class raw_disk_io {
    buffer_pool m_buffer_pool;      // I/O only (128MB = 8192 entries)
    unified_cache m_cache;          // Separate (512MB = 32768 entries)

    raw_disk_io(io_context& ioc, settings_interface const& sett, counters& c)
        : m_buffer_pool(ioc),
          m_cache(calculate_cache_entries(sett))
    {
    }

    void settings_updated() override {
        size_t new_entries = calculate_cache_entries(*m_settings);
        m_cache.set_max_entries(new_entries);

        spdlog::info("Cache resized: {} entries ({} MB)",
                     new_entries, (new_entries * 16) / 1024);
    }

private:
    static size_t calculate_cache_entries(settings_interface const& sett) {
        // cache_size unit is KiB (libtorrent convention)
        int cache_kb = sett.get_int(settings_pack::cache_size);
        // 16KiB per entry
        return (cache_kb * 1024) / 16384;
    }
};

// Example configurations:
// 512MB cache = (512 * 1024 * 1024) / 16384 = 32768 entries
// 1GB cache = (1024 * 1024 * 1024) / 16384 = 65536 entries
// 256MB cache = (256 * 1024 * 1024) / 16384 = 16384 entries
```

### Expected Performance

**Mutex Contention Reduction:**
- Before: 1 mutex, 100% contention with 8+ threads
- After: 32 mutexes, ~3% contention (piece distribution spreads load)

**Lock Duration:**
- Cache hit: ~100ns (hash lookup + memcpy)
- Cache miss with eviction: ~500ns (LRU update + eviction)
- Much faster than disk I/O (ms scale)

**Cache Hit Benefits:**
- Read: Avoid disk read (~5-10ms on HDD, ~0.1ms on SSD)
- Write: Serve recent writes without disk round-trip
- Hash verification: Pieces stay in cache for verification

### Advantages

✅ **Simple implementation** - No complex thread affinity logic
✅ **Automatic load balancing** - boost::asio::thread_pool handles scheduling
✅ **Proven contention reduction** - 32x less contention vs single mutex
✅ **Same-piece locality** - All blocks of piece N in partition N % 32
✅ **Standard libraries** - Uses std::unordered_map, std::mutex

### Disadvantages

⚠️  **Still has mutexes** - Not lock-free, though much better than before
⚠️  **Potential hash collisions** - unordered_map internal buckets (mitigated by good hash function)

## Design Direction B1: Thread Affinity (Backup Plan)

### Concept

**Lock-free through exclusive ownership:**
- Each thread exclusively owns certain partitions
- No other thread touches those partitions → no mutex needed!

```
32 partitions, 8 threads:

Thread 0 owns: partitions 0, 8, 16, 24
Thread 1 owns: partitions 1, 9, 17, 25
Thread 2 owns: partitions 2, 10, 18, 26
...

async_write(piece 100):
    partition_id = 100 % 32 = 4
    thread_id = 4 % 8 = 4
    → Post to thread 4's queue
    → Thread 4 processes without locking!
```

### Implementation Strategy

**Problem:** `boost::asio::thread_pool` does not support posting to specific thread.

**Solution:** Multiple single-threaded `io_context`:

```cpp
class raw_disk_io {
    std::vector<boost::asio::io_context> write_contexts_;  // 8 contexts
    std::vector<std::thread> write_threads_;               // 8 threads

    // Same for read_contexts_, hash_contexts_

    raw_disk_io(int aio_threads) {
        write_contexts_.resize(aio_threads);

        for (int i = 0; i < aio_threads; i++) {
            write_threads_.emplace_back([this, i]() {
                write_contexts_[i].run();  // Each thread runs its own context
            });
        }
    }

    void async_write(storage_index_t storage, peer_request const& r, char const* buf, ...) {
        int partition = r.piece % 32;
        int thread_id = partition % 8;

        // Post to specific thread's io_context
        boost::asio::post(write_contexts_[thread_id], [=, this]() {
            // Now running on fixed thread, no mutex needed!
            partitions_[partition].insert_no_lock({storage, r.piece, r.start}, buf);
        });
    }
};

class cache_partition {
    // NO MUTEX!
    std::unordered_map<location, cache_entry> entries_;
    std::list<location> lru_list_;

public:
    void insert_no_lock(location loc, disk_buffer buf) {
        // Safe because only one thread accesses this partition
        entries_[loc] = {loc, buf, ...};
    }
};
```

### Advantages

✅ **Completely lock-free** - Zero mutex contention
✅ **Deterministic performance** - No lock waiting, no priority inversion
✅ **Better cache locality** - Each thread works on its own partitions

### Disadvantages

❌ **More complex** - Manual thread management instead of thread_pool
❌ **Load imbalance risk** - Some threads may be busier than others
❌ **Requires architecture change** - Can't use existing thread_pool directly

### When to Consider B1

- **Profiling shows** Direction A still has mutex contention issues
- **Workload is uniform** - Pieces distributed evenly across partitions
- **Need deterministic latency** - Real-time requirements

## Memory Management: Method A (Chosen)

**Cache manages its own buffers independently:**

### Why Method A?

**Separation of concerns:**
- `buffer_pool`: Temporary I/O buffers (short-lived, for libtorrent interface)
- `unified_cache`: Persistent cache buffers (long-lived, managed by cache)

**Memory independence:**
- Cache size: 512MB (configurable, e.g., 32768 entries)
- I/O pool size: 128MB (smaller, fixed for I/O operations)
- No competition between cache and I/O for same pool

**Simple ownership:**
- Cache malloc/free its own buffers
- No reference counting needed
- No ownership conflicts with libtorrent

**Rejected: Method B (buffer_pool dependency)**
- Would require zero-copy optimization (complex, ownership conflicts)
- memcpy overhead negligible compared to disk I/O (16KiB copy ~50ns vs 5-10ms disk)
- Couples cache to buffer_pool (unclear responsibility)

## Relationship with store_buffer

**store_buffer will be removed:**

Current (store_buffer):
```cpp
// raw_disk_io.cpp:272
store_buffer_.insert({storage, r.piece, r.start}, buffer.data());
// raw_disk_io.cpp:280 - deleted after write!
store_buffer_.erase({storage, r.piece, r.start});
```

New (unified_cache):
```cpp
// Insert during write (cache malloc's its own buffer)
cache_.insert_write({storage, r.piece, r.start}, buffer.data());
// After write: mark clean but KEEP in cache
cache_.mark_clean({storage, r.piece, r.start});
// Entry stays for future reads!
```

**Benefits:**
- Persistent cache instead of temporary buffer
- Subsequent reads can hit cache
- Write coalescing potential (multiple writes to same block)
- Independent memory management

## Relationship with libtorrent PR #7013

**PR #7013 Design:**
- Has `disk_cache` for write buffering
- Has `store_buffer` for pending writes
- **No read cache** (major performance issue per PR comments)
- Uses high/low watermark for flush

**Our Design Differences:**
- ✅ Unified read/write cache (addresses PR #7013's read cache problem)
- ✅ Sharding for concurrency (PR #7013 has mutex contention issues)
- ✅ Persistent cache entries (not just pending writes)

## Configuration Integration

### From settings_pack

```cpp
// main.cpp
lt::settings_pack p;
int aio_threads = p.get_int(lt::settings_pack::aio_threads);
// Or auto-detect: aio_threads = std::thread::hardware_concurrency();

// raw_disk_io_constructor already receives settings_interface
std::unique_ptr<disk_interface> raw_disk_io_constructor(
    io_context &ioc,
    settings_interface const &sett,
    counters &c)
{
    int aio_threads = sett.get_int(lt::settings_pack::aio_threads);
    return std::make_unique<raw_disk_io>(ioc, aio_threads);
}

// raw_disk_io constructor
raw_disk_io::raw_disk_io(io_context &ioc, int aio_threads) :
    read_thread_pool_(aio_threads),
    write_thread_pool_(aio_threads),
    hash_thread_pool_(aio_threads),
    cache_(read_buffer_pool_, 512)  // 512MB cache
{
}
```

### Command-line Configuration (Future)

```cpp
// config.hpp
struct config {
    size_t cache_size_mb = 512;
    int aio_threads = 0;  // 0 = auto-detect
};

// main.cpp
if (current_config.aio_threads == 0) {
    current_config.aio_threads = std::thread::hardware_concurrency();
}
```

## Hash Function Considerations

### Potential Issue

Current hash uses `boost::hash_combine`:
```cpp
std::size_t hash = 0;
boost::hash_combine(hash, l.torrent);  // Usually 0 or 1
boost::hash_combine(hash, l.piece);    // 0, 1, 2, ...
boost::hash_combine(hash, l.offset);   // 0, 16384, 32768, ... (multiples of 2^14)
```

**Risk:** Offset is always multiple of 16384 (2^14), may cause poor distribution in unordered_map's internal buckets.

### Solution Options

**Option 1: FNV-1a Hash (if needed)**
```cpp
struct hash<torrent_location> {
    std::size_t operator()(argument_type const &l) const {
        size_t h = 2166136261u;
        h = (h ^ static_cast<size_t>(l.torrent)) * 16777619u;
        h = (h ^ static_cast<size_t>(l.piece)) * 16777619u;
        h = (h ^ static_cast<size_t>(l.offset)) * 16777619u;
        return h;
    }
};
```

**Option 2: Trust stdlib** (recommended for Phase 1)
- Modern unordered_map implementations are robust
- Profile first, optimize later
- Avoid premature optimization

## Implementation Phases

### Phase 0: Logging & Debugging (Current Priority)
- 0.1: spdlog runtime log control
- 0.2: set_alert_notify()

### Phase 1: Unified Cache with Sharding (Direction A)
**Goal:** Replace store_buffer with persistent sharded cache

**Tasks:**
1. Create `cache_partition` class (mutex + map + LRU)
2. Create `unified_cache` class (32 partitions)
3. Integrate with `raw_disk_io::async_write`
4. Integrate with `raw_disk_io::async_read`
5. Remove `store_buffer` code
6. Add cache size configuration
7. Add thread pool size configuration from settings_pack

**Estimated Time:** 2-3 days

**Testing:**
- Unit tests for cache_partition
- Integration test: write → read should hit cache
- Performance test: measure mutex contention reduction
- Benchmark: compare with current store_buffer

### Phase 2: Optimization (If Needed)
**Only if profiling shows issues:**
- Improve hash function (FNV-1a)
- Consider Direction B1 (thread affinity)
- Tune partition count dynamically

## Eviction Policy: LRU vs ARC

### Phase 1: LRU (Simple, Proven)

**Implementation:**
- Standard LRU per partition
- `std::list<location>` for LRU ordering
- MRU at front, LRU at back
- On access: move to front
- On eviction: pop from back

**Advantages:**
- ✅ Simple implementation (~50 lines)
- ✅ O(1) access, eviction, promotion
- ✅ Well understood, easy to debug
- ✅ Sufficient for many workloads

### Backup: ARC (Adaptive Replacement Cache)

**When to use:** If profiling shows LRU is insufficient

**Why ARC for BitTorrent:**

1. **Distinguishes access patterns:**
   - **T1 (Recently used once)**: New blocks from network
   - **T2 (Frequently used)**: Blocks accessed multiple times (verification, re-read)

2. **BitTorrent-specific benefits:**
   - Sequential writes: Most blocks used once → stay in T1
   - Random verification: Frequently accessed pieces → promoted to T2
   - Self-tuning: Adapts to ratio of sequential vs random access

3. **Better than LRU for:**
   - Mixed sequential/random workloads
   - Scan resistance (sequential doesn't evict frequently-used)
   - Re-verification patterns (hash checks)

**ARC Structure:**

```cpp
class cache_partition_arc {
    std::mutex mutex_;

    // ARC lists (all fixed 16KiB entries)
    std::list<location> t1_;        // Recent once
    std::list<location> t2_;        // Frequent
    std::list<location> b1_;        // Ghost: recently evicted from T1
    std::list<location> b2_;        // Ghost: recently evicted from T2

    // Actual cache entries
    std::unordered_map<location, cache_entry> entries_;

    // ARC parameter (self-tuning)
    size_t p_;                      // Target size for T1
    size_t max_entries_;            // Total capacity

public:
    void insert(location loc, disk_buffer buf, bool dirty);
    bool get(location loc, disk_buffer& out);
    void mark_clean(location loc);

private:
    void replace(location loc);     // ARC eviction algorithm
    void adapt(bool hit_in_b1);     // Adjust p based on hit pattern
};
```

**ARC Algorithm (Simplified):**

```cpp
bool cache_partition_arc::get(location loc, disk_buffer& out) {
    std::lock_guard lock(mutex_);

    // Case 1: Hit in T1 or T2
    auto it = entries_.find(loc);
    if (it != entries_.end()) {
        // Promote to T2 (frequently used)
        move_to_t2(loc);
        out = it->second.buffer;
        return true;
    }

    // Case 2: Hit in B1 (recently evicted from T1)
    if (in_b1(loc)) {
        adapt(true);   // Increase p (favor recent)
        return false;  // Still need to load from disk
    }

    // Case 3: Hit in B2 (recently evicted from T2)
    if (in_b2(loc)) {
        adapt(false);  // Decrease p (favor frequent)
        return false;
    }

    // Case 4: Complete miss
    return false;
}

void cache_partition_arc::insert(location loc, disk_buffer buf, bool dirty) {
    std::lock_guard lock(mutex_);

    if (entries_.size() >= max_entries_) {
        replace(loc);  // Evict using ARC policy
    }

    // Insert into T1 (recent)
    t1_.push_front(loc);
    entries_[loc] = {loc, buf, dirty, t1_.begin()};
}

void cache_partition_arc::replace(location loc) {
    // ARC eviction algorithm
    if (t1_.size() >= p_) {
        // Evict from T1
        location victim = t1_.back();
        t1_.pop_back();
        entries_.erase(victim);
        b1_.push_front(victim);  // Add to ghost list
    } else {
        // Evict from T2
        location victim = t2_.back();
        t2_.pop_back();
        entries_.erase(victim);
        b2_.push_front(victim);
    }
}

void cache_partition_arc::adapt(bool hit_in_b1) {
    if (hit_in_b1) {
        // Hit in B1 → increase p (favor recency)
        p_ = std::min(p_ + 1, max_entries_);
    } else {
        // Hit in B2 → decrease p (favor frequency)
        p_ = std::max(p_ - 1, size_t(0));
    }
}
```

**ARC Benefits:**
- ✅ **Self-tuning**: No manual parameter tuning
- ✅ **Scan resistance**: Sequential writes don't evict hot data
- ✅ **Better hit rate**: 10-30% improvement vs LRU in mixed workloads
- ✅ **Adaptive**: Learns workload characteristics over time

**ARC Disadvantages:**
- ❌ **More complex**: ~200 lines vs ~50 for LRU
- ❌ **More memory**: Ghost lists (B1, B2) need tracking
- ❌ **Harder to debug**: Four lists + adaptive parameter
- ❌ **Patent concerns**: IBM patent (expired 2010, now safe)

**Implementation Strategy:**

1. **Phase 1.1**: Implement LRU first
2. **Profile**: Measure cache hit rate, eviction patterns
3. **Decision point**: If LRU hit rate < 60% or eviction shows scan issues
4. **Phase 1.2**: Implement ARC as drop-in replacement
   - Same interface as `cache_partition`
   - Just change internal eviction logic
   - Compare hit rates in production

**Metrics to decide LRU vs ARC:**
- Cache hit rate: LRU < 60% → consider ARC
- Eviction patterns: Too many "hot" entries evicted → ARC
- Workload: Heavy verification/re-read → ARC likely better
- Simplicity preference: LRU good enough → keep LRU

### Other Alternatives (Not Recommended)

**CLOCK / Second-Chance:**
- Approximates LRU with single bit per entry
- Simpler than ARC, but not adaptive
- Not worth the complexity vs pure LRU

**LFU (Least Frequently Used):**
- Counts access frequency
- Poor for temporal patterns (old frequent items stay forever)
- Not suitable for BitTorrent (access patterns change over time)

**Random Eviction:**
- Fast, no metadata
- Terrible hit rate
- Never use

## Open Questions

1. **LRU vs ARC decision:**
   - Start with LRU (simple)
   - Measure hit rate in production
   - Switch to ARC if needed (drop-in replacement)

2. **Partition count strategy:**

   **Phase 1.1 (Simple)**: Hardcode 32
   ```cpp
   static constexpr size_t NUM_PARTITIONS = 32;
   ```
   - Fast to implement
   - Good for 4-16 threads
   - Test and validate sharding works

   **Phase 1.2 (Configurable)**: Based on aio_threads
   ```cpp
   class unified_cache {
       std::vector<cache_partition> partitions_;  // Dynamic size

       unified_cache(buffer_pool& pool, size_t cache_mb, int aio_threads) {
           // Rule: 2-4x thread count, minimum 16, power of 2
           size_t num = std::max(16, next_power_of_2(aio_threads * 2));
           partitions_.resize(num);
       }
   };
   ```
   - Determined at startup (read from settings_pack)
   - **Fixed afterwards** (no runtime resize - too complex)
   - Formula options:
     - Conservative: `max(16, aio_threads * 2)`
     - Aggressive: `max(32, aio_threads * 4)`
   - Use power-of-2 for efficient modulo (compiler optimizes to bitwise AND)

   **NOT doing**: Runtime dynamic adjustment
   - Would require rehashing all entries
   - Would need to pause all I/O
   - Complexity >> benefit
   - Set once at startup is sufficient

3. **Eviction policy?**
   - Simple LRU within each partition
   - Or global coordination (more complex)?

4. **Write coalescing?**
   - If same block written multiple times before flush
   - Currently out of scope, but cache enables this

## References

- libtorrent PR #7013: pread-disk-io backend
- Current store_buffer implementation: `store_buffer.hpp`
- Current raw_disk_io: `raw_disk_io.cpp` lines 240-296
