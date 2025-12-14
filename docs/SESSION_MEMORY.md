# EZIO Project Session Memory - Complete Conversation Summary

**Date**: 2024-12-14
**Status**: In-depth analysis complete, ready for implementation
**Note**: This document is the single source of truth (English). A Traditional Chinese translation is provided in `SESSION_MEMORY_zh.md` for human reading.

---

## Part 1: Critical Architecture Discovery

### EZIO's True Architecture (Important!)

**Core Understanding**: EZIO operates on **Raw Disk**, with no filesystem!

```
Previous Misunderstanding:
- EZIO uses regular filesystems (ext4/NTFS, etc.)
- Need to handle file boundaries
- Need to query filesystem's physical layout (FIEMAP)

Correct Understanding (Current):
- EZIO directly reads/writes raw disk (e.g., /dev/sda1)
- Torrent "files" are just disk offset definitions
- Filename = hex representation of offset (e.g., "0x00000000")
- BitTorrent piece → directly calculates corresponding disk offset
```

**Data Flow**:
```
BitTorrent peer sends: piece 5, block 0 (16KB)
    ↓
Calculate disk offset:
    disk_offset = piece_id × piece_size + block_offset
    Example: 5 × 1MB + 0 = 0x500000
    ↓
Write directly to raw disk:
    pwrite(disk_fd, buffer, 16KB, 0x500000)
```

**Key Properties**:
1. ✅ **No filesystem boundaries**: Entire disk is contiguous
2. ✅ **Simple offset calculation**: Pure arithmetic
3. ✅ **Naturally aligned**: 16KB blocks align to 512/4096 byte sectors
4. ✅ **Guaranteed contiguity**: Blocks within same piece are contiguous on disk
5. ✅ **No FIEMAP needed**: No filesystem queries required

### Impact of This Discovery

**Simplified Aspects**:
- Write coalescing becomes trivially simple (just compare offsets)
- No need to consider cross-file merging
- No need to handle file fragmentation
- No need for special ioctls

**New Opportunities**:
- Easy to implement write coalescing
- Can consider O_DIRECT (already aligned)
- Can maximize HDD sequential write performance

---

## Part 2: libtorrent 2.x Source Code Analysis

### Research Source
- **Location**: `tmp/libtorrent-2.0.10/`
- **Version**: v2.0.10
- **Method**: Direct source code reading

### Key Findings

#### 1. Buffer Pool Design

**libtorrent 2.x**:
```cpp
// src/mmap_disk_io.cpp:327
struct mmap_disk_io {
    aux::disk_buffer_pool m_buffer_pool;  // ← Single unified pool!
    // No separate read/write pools
};
```

**EZIO**:
```cpp
// raw_disk_io.hpp:24-25
class raw_disk_io {
    buffer_pool read_buffer_pool_;   // 128 MB
    buffer_pool write_buffer_pool_;  // 128 MB
    // ← Separated! This is EZIO's design decision, not libtorrent's
};
```

**Conclusion**: EZIO diverged from libtorrent 2.x design

**Impact**:
- 42% memory waste during unbalanced workloads
- Read-heavy: read pool full, but write pool has 100MB idle
- Write-heavy: write pool full, but read pool has 100MB idle

#### 2. store_buffer Design

**libtorrent 2.x**:
```cpp
// include/libtorrent/aux_/store_buffer.hpp
struct store_buffer {
    std::unordered_map<torrent_location, char const*> m_store_buffer;
    std::mutex m_mutex;
    // ... get(), insert(), erase()
};
```

**EZIO**:
```cpp
// store_buffer.hpp
class store_buffer {
    std::unordered_map<torrent_location, char const*> m_store_buffer;
    std::mutex m_mutex;
    // ... Identical!
};
```

**Conclusion**: ✅ EZIO correctly copied libtorrent's store_buffer

#### 3. Write Path

**libtorrent 2.x**:
```cpp
// src/mmap_disk_io.cpp:677-713
status_t do_write(mmap_disk_job* j) {
    // Single buffer write
    int ret = j->storage->write(m_settings, b, j->piece, j->d.io.offset, ...);

    // Erase from cache immediately after write completes
    m_store_buffer.erase({storage, piece, offset});
}

// src/mmap_storage.cpp:607-696
int mmap_storage::write(...) {
    // Uses single pwrite(), not pwritev()
    return aux::pwrite_all(handle->fd(), buf, file_offset, ec.ec);
}
```

**Findings**:
1. ❌ **No write coalescing**: One pwrite() per 16KB block
2. ⚠️ **Immediate cache eviction**: Erase after write
3. ✅ **This is libtorrent 2.x design**: Not a bug, intentional

**EZIO**: Same behavior as libtorrent 2.x

#### 4. Settings System

**libtorrent 2.x**:
```cpp
// src/mmap_disk_io.cpp:498-510
void mmap_disk_io::settings_updated() {
    // Update buffer pool
    m_buffer_pool.set_settings(m_settings);

    // Update file pool
    m_file_pool.resize(m_settings.get_int(settings_pack::file_pool_size));

    // Update thread pools
    int num_threads = m_settings.get_int(settings_pack::aio_threads);
    m_generic_threads.set_max_threads(num_threads);
}

// src/disk_buffer_pool.cpp:198-213
void disk_buffer_pool::set_settings(settings_interface const& sett) {
    int pool_size = std::max(1,
        sett.get_int(settings_pack::max_queued_disk_bytes) / default_block_size);

    m_max_use = pool_size;
    m_low_watermark = m_max_use / 2;
}
```

**EZIO**:
```cpp
// raw_disk_io.cpp:464-466
void raw_disk_io::settings_updated() {
    // Empty! Needs implementation
}

// raw_disk_io.cpp:114-119
std::unique_ptr<disk_interface> raw_disk_io_constructor(
    io_context& ioc,
    settings_interface const& s,  // ← Received
    counters& c)                   // ← Received
{
    return std::make_unique<raw_disk_io>(ioc);  // ← But not passed!
}
```

**Problems**:
1. ❌ `settings_updated()` is empty implementation
2. ❌ Constructor doesn't receive `settings_interface&`
3. ❌ `buffer_pool` has no `set_settings()` method

---

## Part 3: Technical Decisions Summary

### Decision 1: Buffer Pool Merger ✅

**Decision**: Merge `read_buffer_pool_` and `write_buffer_pool_` into single `unified_buffer_pool_`

**Rationale**:
1. Align with libtorrent 2.x design
2. Solve memory waste in unbalanced workloads
3. Simplify code

**Benefits**:
- Read-heavy workload: 58% → 86% memory efficiency (+48%)
- Write-heavy workload: 58% → 86% memory efficiency (+48%)
- Balanced workload: maintains 100%

**Implementation**: See `docs/BUFFER_POOL_MERGER.md`

### Decision 2: Configurable Cache Size ✅

**Decision**: Implement `settings_updated()` and `buffer_pool::set_settings()`

**Rationale**:
1. Production environment requirement
2. Different workloads need different cache sizes
3. libtorrent already has configuration mechanism

**Implementation Steps**:
1. Modify `raw_disk_io_constructor` to pass `settings_interface&`
2. Modify `raw_disk_io` constructor to receive and store reference
3. Implement `raw_disk_io::settings_updated()`
4. Add `buffer_pool::set_settings()`

**Implementation**: See `docs/CACHE_SIZE_CONFIG.md`

### Decision 3: Per-Thread Cache ❌

**Decision**: **Do NOT** use per-thread cache, maintain global shared `store_buffer_`

**Rationale**:
1. ✅ Current design is correct (global + mutex)
2. ✅ Any thread can access any cached block
3. ❌ Per-thread cache causes cross-thread cache misses
4. ❌ Memory waste (duplicate storage)
5. ❌ Requires cache coherency, high complexity

**User Question**:
> "Will async_read access cache from a different thread?"

**Answer**:
- Current design: Global `store_buffer_` with mutex protection
- Write thread 1 writes → store_buffer_.insert()
- Read thread 2 reads → store_buffer_.get() → **Success!**
- Cross-thread access works fine

### Decision 4: Write Coalescing ✅

**Decision**: Implement write coalescing using `store_buffer_` to delay flush

**Method**: Raw Disk simplified version (no filesystem queries needed)

**Key Insight** (from user):
> "If we have cache, flush adjacent blocks together"

**Design**:
1. `async_write()` puts data in `store_buffer_`
2. Don't write immediately, add to `pending_writes_`
3. Accumulate contiguous blocks (same piece)
4. Trigger conditions:
   - Accumulated 64 blocks (1 MB)
   - Timeout 100ms
   - Piece complete
   - Next block not contiguous
5. Flush using `pwritev()` for single write

**Benefits**:
- HDD: 73% performance improvement (reduce seeks)
- SSD: 20-30% performance improvement (reduce syscalls)
- Side effect: Extends cache retention time (partially solves Issue 2)

**Complexity**:
- ✅ 10x simpler than expected (due to raw disk)
- ✅ No FIEMAP needed
- ✅ No file boundary handling
- ✅ Offset calculation trivial

### Decision 5: io_uring 🤔

**Decision**: Optional, not required, but can be considered (if complexity manageable)

**Conditions**:
1. Don't use O_DIRECT (avoid alignment issues)
2. Keep buffered I/O
3. Only use io_uring to reduce syscall overhead

**Expected Benefits**:
- Additional 20-30% syscall reduction
- Benefits stack with write coalescing

**Priority**: Low (complete first 3 optimizations first)

---

## Part 4: Write Coalescing Detailed Design

### Data Structures

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public libtorrent::disk_interface {
private:
    store_buffer store_buffer_;  // Existing, data cache

    // NEW: Pending writes for flush
    struct pending_write {
        torrent_location location;      // (storage, piece, offset)
        char const* buffer;             // Points to store_buffer_ data
        uint64_t disk_offset;           // Actual raw disk offset
        std::function<void(storage_error const&)> handler;
        time_point enqueue_time;        // For timeout checking
    };

    // Group by storage (each storage = one disk/partition)
    std::map<storage_index_t, std::vector<pending_write>> pending_writes_;

    std::mutex pending_mutex_;
    boost::asio::steady_timer flush_timer_;

    // Configuration
    struct write_coalesce_config {
        size_t max_pending_blocks = 64;         // Max 64 blocks (1MB)
        std::chrono::milliseconds timeout = 100ms;
        size_t min_coalesce_count = 4;          // At least 4 blocks worth merging
        bool enable = true;
    } coalesce_config_;
};
```

### async_write Improved Flow

```cpp
bool raw_disk_io::async_write(
    storage_index_t storage,
    peer_request const& r,
    char const* buf,
    std::shared_ptr<disk_observer> o,
    std::function<void(storage_error const&)> handler,
    disk_job_flags_t flags)
{
    // 1. Allocate buffer, copy data (existing logic)
    bool exceeded = false;
    char* write_buffer = write_buffer_pool_.allocate_buffer(exceeded, o);
    if (!write_buffer) return true;

    std::memcpy(write_buffer, buf, r.length);

    // 2. Put in store_buffer (existing logic)
    torrent_location loc(storage, r.piece, r.start);
    store_buffer_.insert(loc, write_buffer);

    // 3. NEW: Calculate disk offset
    auto& ps = storages_[storage];
    uint64_t disk_offset = ps->calculate_disk_offset(r.piece, r.start);

    // 4. NEW: Add to pending writes (instead of immediate write)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        pending_writes_[storage].push_back({
            loc,
            write_buffer,
            disk_offset,
            std::move(handler),
            std::chrono::steady_clock::now()
        });

        // 5. Check if should flush
        auto& pending = pending_writes_[storage];

        bool should_flush =
            pending.size() >= coalesce_config_.max_pending_blocks ||
            is_piece_complete(storage, r.piece) ||
            !is_next_block_contiguous(pending, disk_offset);

        if (should_flush) {
            flush_pending_writes(storage);
        } else {
            schedule_flush_timer(storage, coalesce_config_.timeout);
        }
    }

    // 6. Return immediately (data in store_buffer, libtorrent satisfied)
    return exceeded;
}
```

### Contiguity Check (Trivial)

```cpp
bool raw_disk_io::is_next_block_contiguous(
    std::vector<pending_write> const& pending,
    uint64_t new_disk_offset) const
{
    if (pending.empty()) return true;

    auto& last = pending.back();
    uint64_t expected = last.disk_offset + DEFAULT_BLOCK_SIZE;

    // Direct disk offset comparison!
    return new_disk_offset == expected;
}
```

### Flush Implementation

```cpp
void raw_disk_io::flush_pending_writes(storage_index_t storage)
{
    std::vector<pending_write> writes;

    // 1. Get all pending writes
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_writes_.find(storage);
        if (it == pending_writes_.end() || it->second.empty()) return;

        writes = std::move(it->second);
        pending_writes_.erase(it);
    }

    // 2. Sort by disk_offset
    std::sort(writes.begin(), writes.end(),
        [](auto& a, auto& b) { return a.disk_offset < b.disk_offset; });

    // 3. Group contiguous blocks
    std::vector<std::vector<pending_write>> groups;
    groups.push_back({writes[0]});

    for (size_t i = 1; i < writes.size(); ++i) {
        auto& last = groups.back().back();

        if (writes[i].disk_offset == last.disk_offset + DEFAULT_BLOCK_SIZE) {
            // Contiguous! Add to current group
            groups.back().push_back(writes[i]);
        } else {
            // Not contiguous, start new group
            groups.push_back({writes[i]});
        }
    }

    // 4. Write each group using pwritev
    for (auto& group : groups) {
        if (group.size() >= coalesce_config_.min_coalesce_count) {
            dispatch_coalesced_write(storage, group);  // Merged write
        } else {
            for (auto& w : group) {
                dispatch_single_write(storage, w);     // Individual write
            }
        }
    }
}
```

### Coalesced Write Implementation

```cpp
void raw_disk_io::dispatch_coalesced_write(
    storage_index_t storage,
    std::vector<pending_write> const& writes)
{
    boost::asio::post(write_thread_pool_, [this, storage, writes]() {
        auto& ps = storages_[storage];
        int fd = ps->get_disk_fd();  // Raw disk fd

        // Prepare iovec
        std::vector<iovec> iov(writes.size());
        for (size_t i = 0; i < writes.size(); ++i) {
            iov[i].iov_base = const_cast<char*>(writes[i].buffer);
            iov[i].iov_len = DEFAULT_BLOCK_SIZE;
        }

        // Write all contiguous blocks in one call!
        uint64_t start_offset = writes[0].disk_offset;
        ssize_t written = pwritev(fd, iov.data(), iov.size(), start_offset);

        storage_error error;
        if (written != (ssize_t)(writes.size() * DEFAULT_BLOCK_SIZE)) {
            error.ec = errno;
            error.operation = operation_t::file_write;
        }

        // Remove from store_buffer (write complete)
        for (auto& w : writes) {
            store_buffer_.erase(w.location);
        }

        // Free buffers
        for (auto& w : writes) {
            write_buffer_pool_.free_disk_buffer(const_cast<char*>(w.buffer));
        }

        // Call all handlers
        for (auto& w : writes) {
            boost::asio::post(ioc_, [handler = w.handler, error]() {
                handler(error);
            });
        }
    });
}
```

---

## Part 5: Implementation Priority & Timeline

### Phase 1: Infrastructure (Must Complete First)

#### 1.1 Buffer Pool Merger
- **Effort**: 1-2 days
- **Benefit**: +48% memory efficiency for unbalanced workloads
- **Risk**: Low
- **Status**: Design complete
- **Document**: `docs/BUFFER_POOL_MERGER.md`

**Files to modify**:
- `buffer_pool.hpp`: Update `MAX_BUFFER_POOL_SIZE` to 256 MB
- `raw_disk_io.hpp`: Remove `write_buffer_pool_`, rename to `unified_buffer_pool_`
- `raw_disk_io.cpp`: Update all allocate/free calls

#### 1.2 Configurable Cache Size
- **Effort**: 1 day
- **Benefit**: Production requirement
- **Risk**: Low
- **Status**: Design complete
- **Document**: `docs/CACHE_SIZE_CONFIG.md`

**Files to modify**:
- `raw_disk_io.hpp`: Add `settings_` and `stats_counters_` members
- `raw_disk_io.cpp`:
  - Update `raw_disk_io_constructor` to pass parameters
  - Update constructor to receive parameters
  - Implement `settings_updated()`
- `buffer_pool.hpp`: Add `set_settings()` method

### Phase 2: Write Coalescing (Performance)

#### 2.1 Basic Write Coalescing
- **Effort**: 2-3 days
- **Benefit**: HDD +73%, SSD +20-30%
- **Risk**: Medium
- **Status**: Design complete

**Day 1: Data structures + basic logic**
- Add `pending_write` struct
- Add `pending_writes_` map
- Modify `async_write()` to delay write

**Day 2: Flush logic**
- Implement `flush_pending_writes()`
- Implement `dispatch_coalesced_write()`
- Implement contiguity checking

**Day 3: Timer + error handling**
- Implement `schedule_flush_timer()`
- Complete error handling
- Handle session shutdown
- Handle piece complete

#### 2.2 Testing & Tuning
- **Effort**: 1-2 days
- Unit tests
- Integration tests
- Performance testing (HDD vs SSD)
- Parameter tuning

### Phase 3: Advanced Optimizations (Optional)

#### 3.1 io_uring Integration
- **Effort**: 1-2 weeks
- **Condition**: Phase 1 & 2 complete, still have performance needs
- **Method**: Buffered I/O (no O_DIRECT)
- **Benefit**: Additional 20-30% syscall reduction

#### 3.2 Adaptive Configuration
- **Effort**: 3-5 days
- Adjust parameters based on disk type
- Dynamically adjust based on workload
- Performance monitoring and auto-tuning

---

## Part 6: Performance Expectations

### Memory Efficiency

| Workload | Current | After Phase 1 | Improvement |
|----------|---------|---------------|-------------|
| Balanced (128R+128W) | 100% | 100% | - |
| Read-heavy (200R+20W) | 58% | 86% | **+48%** |
| Write-heavy (20R+200W) | 58% | 86% | **+48%** |

### HDD Write Performance

| Scenario | Current | After Phase 2 | Improvement |
|----------|---------|---------------|-------------|
| 4 blocks separate | 49ms | 13ms | **-73%** |
| Avg syscall/block | 1.0 | 0.25 | **-75%** |
| Avg seek/block | 12ms | 3ms | **-75%** |

### SSD Write Performance

| Scenario | Current | After Phase 2 | Improvement |
|----------|---------|---------------|-------------|
| Syscall overhead | Baseline | -75% | **+20-30%** |
| Latency | Baseline | -20% | **+20%** |

---

## Part 7: Document Status

### Completed Documents

| File | Status | Content |
|------|--------|---------|
| `CLAUDE.md` | ✅ Complete | Main analysis (based on libtorrent 2.x source) |
| `docs/BUFFER_POOL_MERGER.md` | ✅ Complete | Buffer pool merger detailed plan |
| `docs/CACHE_SIZE_CONFIG.md` | ✅ Complete | Configurable cache size guide |
| `docs/APP_LEVEL_CACHE.md` | ✅ Complete | Application-level cache analysis |
| `docs/APP_LEVEL_CACHE_zh.md` | ✅ Complete | Traditional Chinese translation |
| `docs/HDD_OPTIMIZATION.md` | ✅ Complete | HDD optimization strategies |
| `docs/HDD_OPTIMIZATION_zh.md` | ✅ Complete | Traditional Chinese translation |
| `docs/CONCURRENCY_ANALYSIS.md` | ✅ Complete | Concurrency analysis |
| `docs/CONCURRENCY_ANALYSIS_zh.md` | ✅ Complete | Traditional Chinese translation |
| `docs/CACHE_BRANCH_ANALYSIS.md` | ✅ Complete | Post-mortem of previous cache impl |
| `docs/CACHE_BRANCH_ANALYSIS_zh.md` | ✅ Complete | Traditional Chinese translation |
| `tmp/libtorrent-2.0.10/` | ✅ Retained | libtorrent source for reference |
| `docs/SESSION_MEMORY.md` | ✅ This document | Complete conversation memory (English SSOT) |

### Pending Documents

| File | Priority | Content |
|------|----------|---------|
| `docs/SESSION_MEMORY_zh.md` | High | Traditional Chinese translation |
| `docs/WRITE_COALESCING.md` | High | Write coalescing detailed implementation |
| `docs/WRITE_COALESCING_zh.md` | High | Traditional Chinese translation |
| `docs/IMPLEMENTATION_GUIDE.md` | Medium | Step-by-step implementation guide |
| `docs/TESTING_PLAN.md` | Medium | Testing plan |
| `docs/PERFORMANCE_ANALYSIS.md` | Low | Post-implementation performance analysis |

---

## Part 8: Important Reminders

1. **Language**: User requests communication in Traditional Chinese (Taiwan)
2. **Architecture**: Raw disk, not filesystem
3. **Simplification**: Many previously complex things become simple due to raw disk
4. **Alignment**: 16KB blocks naturally aligned, no worries
5. **Contiguity**: Blocks within same piece guaranteed contiguous on disk
6. **Documentation**: English as SSOT, Traditional Chinese translations for human reading
7. **Location**: Design documents in `docs/`, except `CLAUDE.md` in root

---

## Summary

This conversation deeply analyzed the EZIO project, discovered the key architectural feature (raw disk), and identified three optimization directions:

1. **Buffer Pool Merger** (1-2 days, +48% memory efficiency)
2. **Configurable Cache Size** (1 day, production requirement)
3. **Write Coalescing** (2-3 days, HDD +73% performance)

All designs are complete and ready for implementation.

**Next Step**: Awaiting user confirmation to start Phase 1.1 implementation or other instructions.

---

**Document Version**: 1.0
**Last Updated**: 2024-12-14
**Status**: Complete memory, ready for implementation
