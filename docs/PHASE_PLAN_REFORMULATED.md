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

## Phase Plan

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

