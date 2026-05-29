# Multi-LRU Cache Design

**Version:** 1.0
**Date:** 2025-12-21
**Status:** Design Phase
**Reference:** libtorrent 1.2 block_cache.hpp

---

## Problem Statement

Current single-LRU cache has **73% eviction rate** (708K evictions / 970K inserts), causing performance degradation on NVMe. The main issue is that eviction scans up to 32 entries to find a clean (non-dirty) block, making eviction O(N) instead of O(1).

**Root Cause:**
- Dirty blocks are pinned during async writes
- Single LRU mixes dirty and clean entries
- Eviction must scan to skip dirty blocks
- High eviction rate makes scanning overhead significant

---

## Design Goals

1. **O(1) Eviction**: Separate clean entries for instant eviction
2. **Adaptive Caching**: Promote frequently accessed blocks to higher priority
3. **Watermark Control**: Backpressure mechanism when cache is under pressure
4. **Performance Analysis**: Track per-LRU statistics for tuning

---

## Architecture Overview

### Three LRU Lists

Based on libtorrent 1.2 design, implement three separate LRU lists per partition:

```
┌─────────────────────────────────────────────────────────────┐
│                      Cache Partition                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   │
│  │  write_lru   │   │  read_lru1   │   │  read_lru2   │   │
│  │   (dirty)    │   │  (read once) │   │ (frequent)   │   │
│  └──────────────┘   └──────────────┘   └──────────────┘   │
│         ↑                  ↑                  ↑            │
│         │                  │                  │            │
│    async_write        first read         second read      │
│         │                  │                  │            │
│         └──────────────────┴──────────────────┘            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**List Purposes:**

1. **write_lru** (dirty blocks):
   - Blocks inserted by `async_write()`
   - All blocks are dirty (pending disk write)
   - **Never evicted** until write completes
   - When write completes: move to read_lru2 (assume write → read pattern)

2. **read_lru1** (read once):
   - Blocks inserted by `async_read()` cache miss
   - Accessed only once so far
   - **Low priority** for eviction (evicted before read_lru2)
   - On second access: promote to read_lru2

3. **read_lru2** (frequently accessed):
   - Blocks promoted from read_lru1 (accessed 2+ times)
   - Blocks moved from write_lru after write completes
   - **High priority** (kept longer)
   - **First eviction target** when under pressure (still clean)

---

## Data Structure Design

### Modified cache_entry

```cpp
enum class cache_state : uint8_t {
    write_lru = 0,    // dirty, in write_lru list
    read_lru1 = 1,    // clean, in read_lru1 list (read once)
    read_lru2 = 2     // clean, in read_lru2 list (frequent)
};

struct cache_entry {
    torrent_location location;
    char *buffer;
    bool dirty;                // true = write_lru, false = read_lru1/2
    cache_state state;         // which LRU list this entry belongs to

    // Iterator pointing to this entry's position in the LRU list
    // (write_lru, read_lru1, or read_lru2 depending on state)
    std::list<torrent_location>::iterator lru_iterator;
};
```

### cache_partition Structure

```cpp
class cache_partition {
private:
    // Three separate LRU lists
    std::list<torrent_location> m_write_lru;   // dirty blocks
    std::list<torrent_location> m_read_lru1;   // read once
    std::list<torrent_location> m_read_lru2;   // frequently read

    // Hash map: location → entry (unchanged)
    std::unordered_map<torrent_location, cache_entry> m_entries;

    // Statistics per LRU list
    struct lru_stats {
        std::atomic<uint64_t> inserts{0};
        std::atomic<uint64_t> promotions{0};    // moves to higher priority
        std::atomic<uint64_t> demotions{0};     // moves to lower priority
        std::atomic<uint64_t> evictions{0};
        std::atomic<uint64_t> hits{0};
    };

    lru_stats m_write_lru_stats;
    lru_stats m_read_lru1_stats;
    lru_stats m_read_lru2_stats;

    // Watermark tracking
    size_t m_num_dirty{0};     // count of dirty blocks
    size_t m_num_clean{0};     // count of clean blocks

    std::mutex m_mutex;
    size_t m_max_entries;
};
```

---

## State Transition Rules

### 1. Write Path (async_write)

```
New write → insert to write_lru (dirty=true, state=write_lru)
              ↓
         [Async write to disk]
              ↓
    Write completes, mark clean (dirty=false)
              ↓
         Move to read_lru2 (state=read_lru2)
         (Assumption: writes are likely to be read soon)
```

### 2. Read Path (async_read)

**Case A: Cache miss**
```
Cache miss → read from disk → insert to read_lru1 (dirty=false, state=read_lru1)
```

**Case B: Hit in write_lru**
```
Hit in write_lru (dirty block) → touch write_lru (move to front)
                                 → m_write_lru_stats.hits++
```

**Case C: Hit in read_lru1 (first repeat access)**
```
Hit in read_lru1 → promote to read_lru2 (state=read_lru2)
                 → m_read_lru1_stats.promotions++
                 → m_read_lru2_stats.inserts++
```

**Case D: Hit in read_lru2 (already frequent)**
```
Hit in read_lru2 → touch read_lru2 (move to front)
                 → m_read_lru2_stats.hits++
```

---

## Eviction Strategy

### Priority Order (when cache is full)

```
Priority 1 (evict first):  read_lru1 (LRU end)  ← least recently used, low priority
Priority 2 (evict second): read_lru2 (LRU end)  ← more valuable, evict only if lru1 empty
Priority 3 (never evict):  write_lru            ← dirty blocks, cannot evict
```

### Eviction Algorithm

```cpp
bool cache_partition::evict_one() {
    // Try read_lru1 first (lowest priority)
    if (!m_read_lru1.empty()) {
        auto loc = m_read_lru1.back();  // LRU end
        auto it = m_entries.find(loc);

        // Free buffer and erase entry
        m_buffer_pool.free_buffer(it->second.buffer);
        m_entries.erase(it);
        m_read_lru1.pop_back();

        m_num_clean--;
        m_read_lru1_stats.evictions++;
        return true;  // O(1) eviction!
    }

    // Try read_lru2 if lru1 is empty
    if (!m_read_lru2.empty()) {
        auto loc = m_read_lru2.back();  // LRU end
        auto it = m_entries.find(loc);

        m_buffer_pool.free_buffer(it->second.buffer);
        m_entries.erase(it);
        m_read_lru2.pop_back();

        m_num_clean--;
        m_read_lru2_stats.evictions++;
        return true;  // O(1) eviction!
    }

    // All entries are dirty (in write_lru) - cannot evict
    return false;
}
```

**Key Improvement:** No scanning! Eviction is now **O(1)** instead of O(N).

---

## Cache Watermark Mechanism

### Purpose

Prevent cache from becoming 100% dirty blocks (no space for new reads). Implement backpressure similar to libtorrent 2.x's exceeded checking.

### Design

**Per-Partition Watermarks:**

```cpp
struct cache_partition {
    // Watermark thresholds
    static constexpr float DIRTY_HIGH_WATERMARK = 0.90f;  // 90% dirty = stop writes
    static constexpr float DIRTY_LOW_WATERMARK = 0.70f;   // 70% dirty = resume writes

    // Watermark state
    std::atomic<bool> m_exceeded{false};  // true = cache under pressure

    bool check_watermark() const {
        float dirty_ratio = (float)m_num_dirty / m_max_entries;

        if (!m_exceeded && dirty_ratio > DIRTY_HIGH_WATERMARK) {
            return false;  // Exceeded! Notify libtorrent to pause writes
        }

        if (m_exceeded && dirty_ratio < DIRTY_LOW_WATERMARK) {
            return true;   // Recovered! Resume writes
        }

        return !m_exceeded;  // Current state
    }
};
```

**Integration with async_write:**

```cpp
bool raw_disk_io::async_write(...) {
    auto partition = get_partition(storage, piece);

    // Check watermark before accepting write
    if (!partition->check_watermark()) {
        // Cache is under pressure - notify observer (disk_observer)
        // Libtorrent will pause this peer until cache recovers
        return true;  // true = exceeded, stop writing
    }

    // Watermark OK - proceed with write
    partition->insert(location, buffer);
    post_write_job(...);
    return false;  // false = not exceeded, continue writing
}
```

**Watermark Recovery:**

When write completes and block moves from write_lru → read_lru2:
- `m_num_dirty--`
- `m_num_clean++`
- If dirty ratio drops below LOW_WATERMARK, notify observers to resume

---

## Performance Statistics

### Per-LRU Statistics

Track these metrics for each LRU list:

```cpp
struct lru_performance {
    uint64_t inserts;         // blocks inserted into this list
    uint64_t hits;            // cache hits in this list
    uint64_t promotions;      // moves to higher priority list
    uint64_t demotions;       // moves to lower priority (if any)
    uint64_t evictions;       // blocks evicted from this list
    uint64_t size;            // current number of entries
};
```

### Logging Output

```
=== Multi-LRU Cache Performance ===
Partition 0:
  write_lru:  12,450 entries (47.3%) | 156,234 inserts | 45,123 hits | 12,340 → lru2
  read_lru1:   8,320 entries (31.6%) | 98,432 inserts  | 23,456 hits | 18,234 promoted
  read_lru2:   5,544 entries (21.1%) | 32,123 inserts  | 123,456 hits | 2,345 evicted

  Watermark: 47.3% dirty (OK) | Exceeded: NO

Partition 1:
  ...
```

**Key Metrics:**
- Size distribution: how many blocks in each list
- Promotion rate: how effective is the adaptive logic
- Eviction source: which list is evicting most
- Watermark status: how often we hit exceeded state

---

## Implementation Plan

### Phase 1: Data Structures (2-3 hours)
1. Add `cache_state` enum to cache_entry
2. Add three `std::list<torrent_location>` to cache_partition
3. Add per-LRU statistics structures
4. Add m_num_dirty/m_num_clean counters

### Phase 2: State Transitions (3-4 hours)
1. Modify `insert()` to choose correct LRU list
2. Modify `get()` to implement promotion logic
3. Add `move_to_list()` helper function
4. Update `mark_clean()` to move write_lru → read_lru2

### Phase 3: Eviction Optimization (2 hours)
1. Replace scanning eviction with O(1) logic
2. Try read_lru1 first, then read_lru2
3. Update eviction statistics

### Phase 4: Watermark (2-3 hours)
1. Implement `check_watermark()` per partition
2. Integrate with async_write() return value
3. Add observer notification on recovery
4. Test watermark thresholds (90%/70%)

### Phase 5: Performance Analysis (2 hours)
1. Add per-LRU logging in `log_stats()`
2. Track state transition counts
3. Add watermark exceeded events logging
4. Create comparison with old single-LRU logs

### Phase 6: Testing (3-4 hours)
1. Unit tests for state transitions
2. Unit tests for watermark triggering
3. Integration test with real torrent download
4. Performance comparison (before/after)

**Total Estimated Time:** 14-18 hours

---

## Expected Results

### Performance Improvements

1. **Eviction Latency**: O(N) → O(1)
   - Before: scan up to 32 entries (0.5-2μs per entry = 16-64μs)
   - After: direct eviction (<1μs)

2. **Eviction Success Rate**: 27% → ~95%+
   - Before: fail when all 32 scanned entries are dirty
   - After: fail only when ALL entries are dirty (much rarer)

3. **Cache Efficiency**:
   - Hot blocks stay longer (in read_lru2)
   - Cold blocks evicted faster (from read_lru1)
   - Better adaptation to access patterns

4. **Watermark Benefits**:
   - Prevent 100% dirty scenario
   - Smooth backpressure to libtorrent
   - Better memory management

### Success Criteria

- ✅ Eviction latency < 2μs (vs 16-64μs before)
- ✅ Eviction success rate > 90%
- ✅ No watermark exceeded under normal load
- ✅ NVMe write performance matches or exceeds no-cache baseline

---

## Risks and Mitigations

### Risk 1: Increased Complexity

**Risk:** Three LRU lists are more complex than one
**Mitigation:**
- Clear state transition rules
- Comprehensive unit tests
- Performance logging to validate behavior

### Risk 2: Promotion Overhead

**Risk:** Moving blocks between lists adds CPU cost
**Mitigation:**
- Promotion is O(1) (just update iterators)
- Only promote on second access (not every access)
- Measure overhead with performance counters

### Risk 3: Watermark False Positives

**Risk:** Watermark may trigger unnecessarily
**Mitigation:**
- Tune thresholds based on real workload (90%/70% is starting point)
- Add hysteresis (HIGH=90%, LOW=70%, 20% gap)
- Log watermark events to analyze patterns

---

## Comparison with Alternatives

### Scheme 1: Partition-Based Watermark (Single LRU)
- ❌ Still has O(N) eviction scanning
- ❌ Doesn't solve core eviction problem
- ✅ Simpler to implement

### Scheme 2: Fast Fail + Sync Write Fallback
- ❌ Loses cache benefits for writes
- ❌ Doesn't improve read cache
- ✅ Very simple

### Scheme 3: Store Buffer + Cache Dual Layer
- ✅ Clean separation of concerns
- ❌ More memory overhead (two buffers per block)
- ❌ Duplicate buffering

### Scheme 4: Multi-LRU (This Design)
- ✅ Solves eviction performance problem (O(1))
- ✅ Adaptive caching (promotes hot blocks)
- ✅ Clean watermark integration
- ✅ Proven design (libtorrent 1.2)
- ❌ More complex implementation

**Conclusion:** Scheme 4 is the best long-term solution.

---

## References

- `tmp/libtorrent-1.2/include/libtorrent/block_cache.hpp` - libtorrent 1.2 cache design
- `tmp/libtorrent-2.0.10/src/mmap_disk_io.cpp` - libtorrent 2.x design (no cache)
- `docs/plan/FUTURE_OPTIMIZATIONS.md` - Optimization #6 (write coalescing)
- `docs/SESSION_MEMORY.md` - Phase 3.1 unified cache implementation

---

**Document Status:** Ready for Implementation
**Next Step:** Begin Phase 1 (Data Structures)
