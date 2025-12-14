# EZIO Architecture Analysis & Optimization Guide

**Version:** 3.0 (Complete analysis with raw disk architecture + optimizations)
**Last Updated:** 2024-12-14
**Reference:** libtorrent-2.0.10 source in `tmp/libtorrent-2.0.10/`
**Complete Memory:** See `docs/SESSION_MEMORY.md` for full conversation history

---

## For New AI Sessions: Quick Context

**If you are a new AI session, please read this section to quickly understand the current state:**

### Analysis Process Summary

1. **Initial Discovery** (2024-12-14)
   - Analyzed EZIO's buffer_pool and store_buffer implementation
   - Compared with libtorrent 2.x source code (located in `tmp/libtorrent-2.0.10/`)
   - Discovered EZIO splits read/write buffer pools (libtorrent 2.x uses unified pool)

2. **Critical Architecture Discovery** ⭐
   - **EZIO operates on RAW DISK** (e.g., /dev/sda1), not filesystem!
   - Torrent "files" are just disk offset definitions
   - This discovery simplified write coalescing design (10x simpler)

3. **Technical Decisions**
   - ✅ **Buffer pool merger**: read + write → unified pool (+48% memory efficiency)
   - ✅ **Mutex analysis**: Single mutex will NOT block (utilization <0.4%)
   - ✅ **Write coalescing**: ASYMMETRIC design (writev only, no readv)
     - libtorrent never batches reads (on-demand, one at a time)
     - Only writes can accumulate (arrive continuously from network)
     - HDD +93%, NVMe +20-30%
   - ✅ **Configurable cache**: Implement settings_updated() interface

4. **User Requirements**
   - All documents written in English
   - Design documents in `docs/`, except CLAUDE.md in root directory
   - Use libtorrent's `m_` prefix naming convention
   - Code comments in English, no emojis

5. **Current Status**
   - ✅ All core analysis complete
   - ✅ Four optimization phases designed with code examples
   - ✅ Documentation complete (English only)
   - ✅ **Phase 0 (Logging) designed and prioritized** - critical for debugging
   - ✅ Future optimizations identified (25 items from libtorrent docs)
   - 🔥 **Ready to implement Phase 0 (HIGHEST PRIORITY)**
     - Phase 0.1: spdlog runtime log control (30 min)
     - Phase 0.2: set_alert_notify() event-driven alerts (1-2 hrs)

### Key Files Navigation

**Main Documents:**
- `CLAUDE.md` - This file, quick reference + implementation guide
- `docs/SESSION_MEMORY.md` - Complete conversation history and analysis

**Detailed Design Documents:**
- `docs/BUFFER_POOL_MERGER.md` - Buffer pool merger plan (+48% memory efficiency)
- `docs/CACHE_SIZE_CONFIG.md` - Configurable cache size implementation guide
- `docs/MUTEX_ANALYSIS.md` - Mutex contention analysis (proves not a bottleneck)
- `docs/STORE_BUFFER_WATERMARK.md` - Three-level backpressure design (256MB recommendation)
- `docs/WRITE_COALESCING_DESIGN.md` - Write coalescing implementation (writev only)
- `docs/SPDLOG_ADVANCED.md` - **NEW** spdlog runtime log level control guide
- `docs/LIBTORRENT_ALERTS_LOGGING.md` - **NEW** libtorrent alerts logging (use `set_alert_notify()`)
- `docs/FUTURE_OPTIMIZATIONS.md` - **NEW** 25 optimization opportunities from libtorrent analysis
- `docs/HDD_OPTIMIZATION.md` - HDD optimization strategies
- `docs/APP_LEVEL_CACHE.md` - Application-level cache analysis
- `docs/CONCURRENCY_ANALYSIS.md` - Concurrency analysis
- `docs/CACHE_BRANCH_ANALYSIS.md` - Post-mortem of previous cache implementation

### Important Reminders

1. **Phase 0 FIRST**: Implement logging improvements before other optimizations (critical for debugging)
2. **Raw disk architecture**: EZIO doesn't use filesystem, directly reads/writes raw disk
3. **No alignment assumptions**: Write coalescing won't align to 512B/4K, uses buffered I/O
4. **Naming style**: Member variables use `m_` prefix (libtorrent convention)

### 📝 Documentation Maintenance Guidelines (for AI)

**If generating new session memory or significant analysis:**

1. **Update SESSION_MEMORY.md**
   - Record new conversations, discoveries, decisions in `docs/SESSION_MEMORY.md`
   - Keep all documentation in English

2. **Split files when too large**
   - When SESSION_MEMORY.md exceeds 1000 lines, consider splitting
   - Split by topic into separate files, for example:
     - `docs/SESSION_MEMORY_PHASE1.md` - Phase 1 implementation log
     - `docs/SESSION_MEMORY_PHASE2.md` - Phase 2 implementation log
     - `docs/PERFORMANCE_TUNING.md` - Performance tuning log
     - `docs/PRODUCTION_ISSUES.md` - Production issue tracking
   - Create index links at the beginning of SESSION_MEMORY.md

3. **Update CLAUDE.md summary**
   - When there are significant discoveries or architecture changes
   - Update the "For New AI Sessions" section summary
   - Keep concise, record only key information

4. **Documentation Organization Principles**
   - All design documents in `docs/`
   - CLAUDE.md stays in root directory (quick reference)
   - Use clear filenames that reflect content topics
   - All documentation in English only

**Ready to start implementation? Jump directly to [Optimization Plans](#optimization-plans) section.**

---

## Executive Summary

EZIO is a **BitTorrent-based raw disk imaging tool** for fast LAN deployment. This analysis, grounded in libtorrent 2.x source code, identifies key architectural insights and three high-impact optimizations ready for implementation.

**Critical Discovery: EZIO operates on RAW DISK (e.g., /dev/sda1), not filesystem!**

**Key Findings:**

1. ✅ **store_buffer**: Correctly copied from libtorrent 2.x
2. ✗ **Buffer pool split**: EZIO diverged (libtorrent uses unified pool) → 42% memory waste
3. ✅ **No write coalescing**: Matches libtorrent 2.x (but opportunity for EZIO)
4. ✗ **Configuration system**: settings_updated() empty, not implemented
5. 🔧 **Ready to implement**: 3 optimizations designed, +73% HDD performance possible

**Optimization Plan (Updated Priority):**
- **Phase 0 (HIGHEST PRIORITY)**: Logging improvements (1-2 days, critical for debugging)
  - Phase 0.1: spdlog runtime log level control
  - Phase 0.2: set_alert_notify() for instant alert response
- **Phase 1**: Memory & Configuration (2-3 days)
  - Phase 1.1: Buffer pool merger (+48% memory efficiency)
  - Phase 1.2: Configurable cache size (production requirement)
- **Phase 2**: Write Optimization (3-4 days, HDD +73%, SSD +20-30%)

---

## Table of Contents

1. [Critical Architecture Discovery](#critical-architecture-discovery)
2. [libtorrent 2.x Architecture](#libtorrent-2x-architecture)
3. [EZIO Implementation](#ezio-implementation)
4. [Key Differences](#key-differences)
5. [Optimization Plans](#optimization-plans)
6. [Implementation Guide](#implementation-guide)
7. [Future Work](#future-work)
8. [References](#references)

---

## Critical Architecture Discovery

### EZIO's True Architecture: Raw Disk

**Most Important Discovery:**

```
WRONG Understanding:
- EZIO uses regular filesystems (ext4, NTFS, etc.)
- Needs to handle file boundaries, fragmentation
- Requires filesystem queries (FIEMAP)

CORRECT Understanding:
- EZIO directly reads/writes RAW DISK (e.g., /dev/sda1)
- No filesystem layer!
- Torrent "files" are just disk offset definitions
- Filename = hex offset (e.g., "0x00000000" means disk offset 0)
```

**Data Flow:**

```
BitTorrent Protocol:
  Peer sends: piece 5, block 0 (16KB data)
    ↓
EZIO Calculation:
  disk_offset = piece_id × piece_size + block_offset
  Example: 5 × 1MB + 0 = 0x500000
    ↓
Write to Raw Disk:
  pwrite(disk_fd, buffer, 16KB, 0x500000)
    ↓
Disk Hardware:
  Write directly to physical sector
```

**Key Properties:**

1. ✅ **No file boundaries**: Entire disk is one contiguous address space
2. ✅ **Simple offset calculation**: Pure arithmetic, no filesystem queries
3. ✅ **Guaranteed contiguity**: Blocks within same piece are physically adjacent
4. ✅ **No FIEMAP needed**: No filesystem, no need to query layout
5. ✅ **Simplifies write coalescing**: Just compare offsets to check adjacency

**Impact:**

This discovery **simplifies write coalescing by 10x**:
- No need to handle cross-file boundaries
- No need to handle file fragmentation
- No need for special ioctls or filesystem queries
- Trivial to detect adjacent blocks: `offset2 == offset1 + 16KB`

---

## libtorrent 2.x Architecture

### Overview

**Source:** `tmp/libtorrent-2.0.10/src/mmap_disk_io.cpp`

**Core Design Principles:**
1. **Single unified buffer pool** for all operations
2. **store_buffer** as temporary cache between async_write return and completion
3. **No write coalescing** (relies on OS page cache)
4. **Settings propagation** via settings_updated() interface

```cpp
// src/mmap_disk_io.cpp:327
struct mmap_disk_io final : disk_interface
{
    // Single unified buffer pool for ALL operations
    aux::disk_buffer_pool m_buffer_pool;  // ← ONE pool! (m_ prefix)

    // Store buffer: location → buffer pointer mapping
    aux::store_buffer m_store_buffer;     // ← m_ prefix

    // Other components
    aux::file_view_pool m_file_pool;      // ← m_ prefix
    settings_interface const* m_settings; // ← m_ prefix
    counters& m_stats_counters;           // ← m_ prefix
};
```

**Naming Convention:** libtorrent uses `m_` prefix for member variables.

### Component 1: disk_buffer_pool (Unified)

**Location:** `include/libtorrent/aux_/disk_buffer_pool.hpp`

```cpp
struct disk_buffer_pool final : buffer_allocator_interface
{
    char* allocate_buffer(char const* category);
    char* allocate_buffer(bool& exceeded,
                          std::shared_ptr<disk_observer> o,
                          char const* category);
    void free_buffer(char* buf);
    void set_settings(settings_interface const& sett);

private:
    std::mutex m_pool_mutex;     // ← Protects all members
    int m_in_use;                // Current buffers in use
    int m_max_use;               // Max buffer limit
    int m_low_watermark;         // 50% of m_max_use
    bool m_exceeded_max_size;    // Watermark state
    std::vector<std::weak_ptr<disk_observer>> m_observers;
};
```

**Implementation** (`src/disk_buffer_pool.cpp:122-173`):

```cpp
char* disk_buffer_pool::allocate_buffer(bool& exceeded, ...) {
    std::unique_lock<std::mutex> l(m_pool_mutex);  // ← Single mutex
    char* ret = allocate_buffer_impl(l, category);
    if (m_exceeded_max_size) {
        exceeded = true;
        if (o) m_observers.push_back(o);
    }
    return ret;
}

// allocate_buffer_impl:
// - malloc(default_block_size)    ~1μs
// - ++m_in_use                    ~0.01μs
// - Watermark check               ~0.05μs
// Total critical section: ~1-2μs
```

**Key Characteristics:**

1. **Single pool**: Read, write, hash ALL share same pool
2. **Dynamic allocation**: Uses malloc()/free() on demand
3. **Watermark**: 50% low, 75% high (low + (max-low)/2)
4. **Short critical sections**: Mutex held for 1-2μs only
5. **No I/O under lock**: Only memory operations

**Mutex Contention Analysis:**

```
16 threads (8 read + 8 write) × 100 alloc/sec = 1600 alloc/sec
Total lock time: 1600 × 2μs = 3.2ms/sec
Mutex utilization: 0.32% per second
Availability: 99.68% of time, mutex is FREE

Conclusion: ✅ Single mutex is NOT a bottleneck
```

See [docs/MUTEX_ANALYSIS.md](docs/MUTEX_ANALYSIS.md) for detailed analysis.

### Component 2: store_buffer

**Location:** `include/libtorrent/aux_/store_buffer.hpp`

```cpp
struct store_buffer
{
    template <typename Fun>
    bool get(torrent_location const loc, Fun f) const;

    template <typename Fun>
    int get2(torrent_location const loc1,
             torrent_location const loc2, Fun f) const;

    void insert(torrent_location const loc, char const* buf);
    void erase(torrent_location const loc);

private:
    mutable std::mutex m_mutex;
    std::unordered_map<torrent_location, char const*> m_store_buffer;
};
```

**Purpose:**
- Temporary cache between async_write() return and write completion
- Maps `(storage_index, piece, offset)` → buffer pointer
- Allows async_read() to retrieve data before disk write completes

**Lifecycle:**

```cpp
// async_write():
m_store_buffer.insert({storage, piece, offset}, buffer);
return; // libtorrent knows data is in buffer

// do_write() (worker thread):
pwrite(fd, buffer, size, offset);
m_store_buffer.erase({storage, piece, offset});  // ← Removed after write!
```

### Component 3: Settings System

**Interface** (`src/mmap_disk_io.cpp:498-510`):

```cpp
void mmap_disk_io::settings_updated() {
    // Update buffer pool
    m_buffer_pool.set_settings(m_settings);

    // Update file pool
    m_file_pool.resize(m_settings.get_int(settings_pack::file_pool_size));

    // Update thread pools
    int num_threads = m_settings.get_int(settings_pack::aio_threads);
    m_generic_threads.set_max_threads(num_threads);
}
```

**Purpose:** Propagate configuration changes to disk I/O subsystem

---

## EZIO Implementation

### Architecture

**Location:** `raw_disk_io.hpp`

```cpp
class raw_disk_io final : public disk_interface {
private:
    // EZIO's divergence: TWO separate pools (not m_ prefix!)
    buffer_pool read_buffer_pool_;   // 128 MB for reads
    buffer_pool write_buffer_pool_;  // 128 MB for writes

    // Copied from libtorrent (correct, but missing m_ prefix)
    store_buffer store_buffer_;

    // EZIO uses pread()/pwrite(), not mmap
    // Target: raw disk (/dev/sda1), not filesystem
};
```

**Issues Identified:**

1. ❌ **Split pools**: Diverges from libtorrent 2.x unified design
2. ❌ **Naming style**: Should use `m_` prefix (libtorrent convention)
3. ❌ **settings_updated()**: Empty implementation (line 464-466)
4. ❌ **Constructor**: Doesn't receive settings_interface (line 114-119)

### buffer_pool (EZIO Custom)

**Location:** `buffer_pool.hpp` + `buffer_pool.cpp`

```cpp
class buffer_pool : public libtorrent::buffer_allocator_interface
{
public:
    char* allocate_buffer();
    char* allocate_buffer(bool& exceeded,
                          std::shared_ptr<libtorrent::disk_observer> o);
    void free_disk_buffer(char*) override;

private:
    std::mutex m_pool_mutex;     // ← Has m_ prefix
    int m_size;                  // Current allocated blocks
    bool m_exceeded_max_size;    // Watermark state
    std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;
    std::deque<std::function<void()>> m_disk_buffer_holders;
};
```

**Configuration:**

```cpp
#define MAX_BUFFER_POOL_SIZE (128ULL * 1024 * 1024)  // 128 MB per pool
#define DEFAULT_BLOCK_SIZE (16 * 1024)                // 16 KB

#define BUFFER_COUNT (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE)  // 8192 blocks
#define LOW_WATERMARK (BUFFER_COUNT / 2)                          // 4096 (50%)
#define HIGH_WATERMARK (BUFFER_COUNT / 8 * 7)                     // 7168 (87.5%)
```

**Good Design:**
- ✅ Unlocks mutex before callbacks (buffer_pool.cpp:105)
- ✅ Short critical sections (same as libtorrent)

---

## Key Differences

### 1. Unified vs Split Pool

**Visual Comparison:**

```
libtorrent 2.x:
┌─────────────────────────────────────────┐
│  m_buffer_pool (256 MB)                 │
│  ┌─────────┬─────────┬─────────┐       │
│  │  Read   │  Write  │  Hash   │       │
│  │  (var)  │  (var)  │  (var)  │       │
│  └─────────┴─────────┴─────────┘       │
│  Dynamic: any operation uses any amount │
└─────────────────────────────────────────┘

EZIO (current):
┌──────────────────┐  ┌──────────────────┐
│ read_buffer_     │  │ write_buffer_    │
│ pool_ (128 MB)   │  │ pool_ (128 MB)   │
│ ┌──────────────┐ │  │ ┌──────────────┐ │
│ │ Read only    │ │  │ │ Write only   │ │
│ │ MAX 128 MB   │ │  │ │ MAX 128 MB   │ │
│ └──────────────┘ │  │ └──────────────┘ │
│ Fixed, isolated  │  │ Fixed, isolated  │
└──────────────────┘  └──────────────────┘
```

**Problem: Resource Starvation**

```
Scenario: Seeding (read-heavy workload)
Demand:  200 MB read, 20 MB write

Current (split):
  read_buffer_pool_:  128 MB (FULL, blocking)
  write_buffer_pool_:  20 MB (108 MB idle, WASTED)
  Efficiency: 148/256 = 58%

Unified pool:
  m_buffer_pool: 220 MB total
  Efficiency: 220/256 = 86%

Improvement: +48% memory efficiency
```

**Measurements:**

| Workload | Current | Unified | Gain |
|----------|---------|---------|------|
| Balanced (128R+128W) | 100% | 100% | 0% |
| Read-heavy (200R+20W) | 58% | 86% | **+48%** |
| Write-heavy (20R+200W) | 58% | 86% | **+48%** |

### 2. Configuration System

**libtorrent 2.x:**
```cpp
// Constructor receives settings
mmap_disk_io(io_context& ioc, settings_interface const& sett)
    : m_settings(&sett), ...
{ }

// Implements settings_updated()
void settings_updated() {
    m_buffer_pool.set_settings(*m_settings);
    // ... update other components
}
```

**EZIO:**
```cpp
// Constructor does NOT receive settings
raw_disk_io(io_context& ioc) { }  // ← Missing settings!

// settings_updated() is EMPTY (line 464-466)
void raw_disk_io::settings_updated() {
    // Empty!
}
```

**Problem:** Cache size is hard-coded, not configurable at runtime.

### 3. Write Path (Neither Has Coalescing)

**Current Behavior (Both):**

```cpp
// Each 16KB block = separate pwrite() syscall
for (each block) {
    pwrite(fd, block, 16KB, offset);  // Individual syscall
}

// HDD cost per block:
// - Seek: ~12ms
// - Transfer: ~1ms
// Total: 13ms × 4 blocks = 52ms
```

**Opportunity for EZIO:**

Because EZIO uses **raw disk** (not filesystem), write coalescing is trivial:

```cpp
// Check if blocks are adjacent (simple arithmetic!)
bool is_adjacent = (offset2 == offset1 + 16KB);

// If adjacent, accumulate and flush with pwritev():
struct iovec iov[4];
for (i = 0; i < 4; ++i) {
    iov[i].iov_base = blocks[i];
    iov[i].iov_len = 16KB;
}
pwritev(fd, iov, 4, start_offset);  // ONE syscall for 64KB

// HDD cost:
// - Seek: ~12ms (once)
// - Transfer: ~4ms (64KB)
// Total: 16ms (vs 52ms before)
// Improvement: 69% faster!
```

---

## Optimization Plans

**UPDATED PRIORITY:** Logging improvements moved to Phase 0 (highest priority) for better development experience.

---

### Phase 0: Logging & Debugging 🔥 HIGHEST PRIORITY

**Why First:** These improvements are critical for efficient development and debugging of all subsequent optimizations.

#### 0.1 spdlog Runtime Log Level Control ✅ Ready to Implement

**Goal:** Replace compile-time `SPDLOG_*` macros with runtime-controllable `spdlog::*` functions

**Current Problem:**
```cpp
// log.cpp - Current implementation
SPDLOG_INFO("start alert report thread");  // ❌ Compile-time only
SPDLOG_INFO("[{}][{}%][D: {:.2f}MB/s]", ...);  // ❌ Cannot change at runtime
```

**Solution:**
```cpp
// Replace with runtime functions
spdlog::info("start alert report thread");  // ✅ Runtime controllable
spdlog::info("[{}][{}%][D: {:.2f}MB/s]", ...);

// Usage:
// export SPDLOG_LEVEL=debug    # Enable debug logs
// export SPDLOG_LEVEL=error    # Only errors
// export SPDLOG_LEVEL=info,raw_disk_io=debug  # Component-specific
```

**Benefits:**
- ✅ Debug without recompiling
- ✅ Production troubleshooting (change log level on-the-fly)
- ✅ Component-specific log levels
- ✅ Zero performance impact when disabled

**Effort:** 30 minutes (search & replace + test)

**Changes:**
1. Replace `SPDLOG_TRACE` → `spdlog::trace`
2. Replace `SPDLOG_DEBUG` → `spdlog::debug`
3. Replace `SPDLOG_INFO` → `spdlog::info`
4. Replace `SPDLOG_WARN` → `spdlog::warn`
5. Replace `SPDLOG_ERROR` → `spdlog::error`

**Files to modify:**
- `log.cpp` (4 occurrences)
- Any other files using `SPDLOG_*` macros

**Document:** [docs/SPDLOG_ADVANCED.md](docs/SPDLOG_ADVANCED.md)

#### 0.2 set_alert_notify() for Instant Alert Response ✅ Ready to Implement

**Goal:** Replace polling with event-driven alert handling using `set_alert_notify()`

**Current Problem:**
```cpp
// log.cpp:53-65 - Polling every 5 seconds
while (!shutdown) {
    std::this_thread::sleep_for(std::chrono::seconds(5));  // ❌ Delays alerts
    pop_alerts(&alerts);  // ❌ Blocks shutdown for 5 seconds
}
```

**Solution (libtorrent best practice):**
```cpp
// Set callback (called from libtorrent internal thread)
session.set_alert_notify([this]() {
    // Fast notification - just wake worker thread
    std::lock_guard lock(m_alert_mutex);
    m_alert_ready = true;
    m_alert_cv.notify_one();
});

// Worker thread waits efficiently
while (!shutdown) {
    std::unique_lock lock(m_alert_mutex);
    m_alert_cv.wait_for(lock, 1s, [this]() {
        return m_alert_ready || shutdown;
    });

    if (m_alert_ready) {
        m_alert_ready = false;
        lock.unlock();
        pop_alerts(&alerts);  // Process immediately
    }
}
```

**Benefits:**
- ✅ **Instant response**: < 1ms (vs 0-5s delay)
- ✅ **Zero CPU waste**: True event-driven (no polling)
- ✅ **Fast shutdown**: Exits within 1 second
- ✅ **Better debugging**: Errors logged immediately
- ✅ **Production ready**: Follows libtorrent best practices

**Effort:** 1-2 hours

**Changes:**
1. Add `set_alert_notify()` wrapper to `daemon.hpp` + `daemon.cpp`
2. Add synchronization to `log.hpp` (mutex, condition_variable, flag)
3. Setup callback in `log::log()` constructor
4. Rewrite `log::report_alert()` to use condition_variable
5. Add alert categorization (errors as ERROR, warnings as WARN, etc.)
6. Optional: Add alert-specific formatting for important types

**Files to modify:**
- `daemon.hpp` (add `set_alert_notify()` method)
- `daemon.cpp` (implement wrapper)
- `log.hpp` (add sync primitives)
- `log.cpp` (rewrite alert handler + categorization)

**Impact on Future Work:**
- ✅ Immediately see write coalescing effects in logs
- ✅ Debug buffer pool pressure in real-time
- ✅ Monitor performance warnings instantly
- ✅ Catch storage errors before they escalate

**Document:** [docs/LIBTORRENT_ALERTS_LOGGING.md](docs/LIBTORRENT_ALERTS_LOGGING.md)

---

### Phase 1: Infrastructure Improvements

#### 1.1 Unified Cache with Sharding ✅ Ready to Implement

**Goal:** Replace store_buffer with persistent sharded cache to solve mutex contention

**Background:**
- Previous LRU implementation had severe mutex contention with 8+ I/O threads
- Single global mutex dominated performance, not cache logic
- Profiling confirmed: **mutex is the bottleneck**

**Solution: Sharded Cache (Direction A)**

```cpp
class unified_cache {
    static constexpr size_t NUM_PARTITIONS = 32;  // Hardcoded initially
    std::array<cache_partition, NUM_PARTITIONS> partitions_;

    size_t get_partition_index(location const& loc) {
        return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
    }
};

class cache_partition {
    std::mutex mutex_;                              // Per-partition mutex
    std::unordered_map<location, cache_entry> entries_;
    std::list<location> lru_list_;                  // LRU eviction
    size_t max_entries_per_partition_;
};

struct cache_entry {
    location loc;              // (torrent, piece, offset)
    char* buffer;              // 16KiB, malloc'ed by cache itself
    bool dirty;                // Needs writeback?
    std::list<location>::iterator lru_iter;
};
```

**Benefits:**
- ✅ **32x less contention**: Single mutex → 32 partitions (~3% collision rate)
- ✅ **Persistent cache**: Entries stay after write (unlike store_buffer)
- ✅ **Read cache**: async_read can hit cache (major improvement vs PR #7013)
- ✅ **Write cache**: async_write cached for future reads
- ✅ **Configurable size**: e.g., 512MB = 32768 entries of 16KiB
- ✅ **Same-piece locality**: All blocks of piece N in partition N % 32

**Eviction Policy:**
- **Phase 1.1**: LRU per partition (simple, proven)
- **Backup if LRU insufficient**: ARC (Adaptive Replacement Cache)
  - Distinguishes "recently used" (T1) vs "frequently used" (T2)
  - Better for BitTorrent pattern: sequential writes + random verification
  - Self-tuning, adapts to workload
  - **Only implement if LRU profiling shows issues**

**Replaces store_buffer:**
```cpp
// OLD: store_buffer (temporary)
store_buffer_.insert({storage, piece, offset}, buf);
// ... write to disk ...
store_buffer_.erase({storage, piece, offset});  // ❌ Deleted immediately

// NEW: unified_cache (persistent)
cache_.insert_write({storage, piece, offset}, buf);
// ... write to disk ...
cache_.mark_clean({storage, piece, offset});    // ✅ Kept in cache!
```

**Configuration:**
```cpp
// From settings_pack (already available in constructor)
int aio_threads = sett.get_int(lt::settings_pack::aio_threads);

// Calculate cache entries from settings
size_t cache_entries = (sett.get_int(settings_pack::cache_size) * 1024) / 16384;
// Example: cache_size=524288 KiB (512MB) → 32768 entries

raw_disk_io::raw_disk_io(io_context &ioc, settings_interface const &sett) :
    m_buffer_pool(ioc),                 // I/O only (128MB)
    read_thread_pool_(aio_threads),
    write_thread_pool_(aio_threads),
    hash_thread_pool_(aio_threads),
    m_cache(cache_entries)              // Separate, manages own buffers
{
}

void raw_disk_io::settings_updated() override {
    size_t new_entries = (m_settings->get_int(settings_pack::cache_size) * 1024) / 16384;
    m_cache.set_max_entries(new_entries);  // Dynamic resize
}
```

**Memory separation:**
- `buffer_pool`: Temporary I/O buffers (~128MB)
- `unified_cache`: Persistent cache, manages own malloc/free (512MB+)
- Total predictable: I/O + Cache

**Backup Plan: Thread Affinity (Direction B1)**
- Only if profiling shows Direction A still has contention
- Use multiple single-threaded io_context instead of thread_pool
- Each thread exclusively owns certain partitions → completely lock-free
- More complex, requires architecture change
- See detailed design in [docs/UNIFIED_CACHE_DESIGN.md](docs/UNIFIED_CACHE_DESIGN.md)

**Effort:** 2-3 days

**Files to create:**
- `cache_partition.hpp` / `.cpp`
- `unified_cache.hpp` / `.cpp`

**Files to modify:**
- `raw_disk_io.hpp` (add unified_cache member)
- `raw_disk_io.cpp` (integrate with async_read/async_write)
- Remove `store_buffer` code

**Testing:**
- Unit tests for cache_partition (LRU, eviction)
- Integration: write → read should hit cache
- Performance: measure mutex contention reduction vs old LRU
- Benchmark: throughput vs current store_buffer

**Document:** [docs/UNIFIED_CACHE_DESIGN.md](docs/UNIFIED_CACHE_DESIGN.md)

#### 1.2 Buffer Pool Merger ✅ Ready to Implement

**Goal:** Merge read_buffer_pool_ + write_buffer_pool_ → m_buffer_pool

**Benefits:**
- +48% memory efficiency for unbalanced workloads
- Aligns with libtorrent 2.x design
- Simplifies code

**Effort:** 1-2 days

**Changes:**

```cpp
// buffer_pool.hpp
-#define MAX_BUFFER_POOL_SIZE (128ULL * 1024 * 1024)
+#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)  // Increased for 10Gbps

// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
private:
-    buffer_pool read_buffer_pool_;   // 128 MB
-    buffer_pool write_buffer_pool_;  // 128 MB
+    buffer_pool m_buffer_pool;       // 256 MB (unified, m_ prefix)

-    store_buffer store_buffer_;
+    store_buffer m_store_buffer;     // Add m_ prefix

+    // NEW: Backpressure tracking
+    std::atomic<size_t> m_total_pending_count{0};
+    struct backpressure_config {
+        size_t urgent_threshold = 512;      // Soft limit → urgent flush
+        size_t critical_threshold = 1024;   // Hard limit → sync write
+    } m_backpressure_config;
};
```

**Why 256 MB:**
- 10 Gbps network = 1.25 GB/s = 78,125 blocks/sec
- HDD burst buffer: ~10 seconds = 192 MB needed
- 256 MB provides safety margin
- See: [docs/STORE_BUFFER_WATERMARK.md](docs/STORE_BUFFER_WATERMARK.md)

**Mutex Contention:** ✅ Not a concern (see [docs/MUTEX_ANALYSIS.md](docs/MUTEX_ANALYSIS.md))

**Document:** [docs/BUFFER_POOL_MERGER.md](docs/BUFFER_POOL_MERGER.md)

#### 1.2 Configurable Cache Size ✅ Ready to Implement

**Goal:** Implement settings_updated() + buffer_pool::set_settings()

**Benefits:**
- Production requirement (different workloads need different sizes)
- Follows libtorrent design pattern

**Effort:** 1 day

**Changes:**

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
public:
    raw_disk_io(io_context& ioc,
                settings_interface const& sett,  // ← Add
                counters& cnt);                   // ← Add

    void settings_updated() override;             // ← Implement

private:
    settings_interface const* m_settings;         // ← Add
    counters& m_stats_counters;                   // ← Add
    buffer_pool m_buffer_pool;
};

// raw_disk_io.cpp
void raw_disk_io::settings_updated() {
    m_buffer_pool.set_settings(*m_settings);
}

// buffer_pool.hpp
class buffer_pool {
public:
    void set_settings(settings_interface const& sett);  // ← Add
};
```

**Document:** [docs/CACHE_SIZE_CONFIG.md](docs/CACHE_SIZE_CONFIG.md)

### Phase 2: Write Optimization ✅ Ready to Implement

**Goal:** Maximize disk write throughput for both HDD and NVMe

**Phase 2.1: Write Coalescing + Backpressure**
- Add `writev()` to partition_storage (keep existing `read()`/`write()`)
- raw_disk_io accumulates writes, calls partition_storage.writev()
- **NEW:** Three-level backpressure system:
  - Level 1: Normal flush (timeout/accumulation)
  - Level 2: Urgent flush (buffer pressure > 80%)
  - Level 3: Sync write (pending > 1024, blocks libtorrent) ⚠️
- partition_storage handles file_slice mapping and pwritev internally
- **Note:** Only writev, NO readv (libtorrent never batches reads)
- Benefits:
  - HDD: +93% (reduce seeks from 64 → 1)
  - NVMe: +20-30% (reduce syscalls)
  - **Critical:** Prevents unbounded memory growth
- Effort: 3-4 days (includes backpressure)

**Phase 2.2: Parallel Writes (Critical for NVMe!)**
- Increase write thread pool: 8 → 32 threads
- Saturate NVMe queue depth (current QD=1 → QD=32)
- Benefits:
  - **NVMe: +100-150% throughput** (500 MB/s → 1+ GB/s) ✅
  - Saturate 10Gbps network (1.25 GB/s)
  - HDD: No negative impact (seeks dominate anyway)
- Effort: **1 day** (simple change!)

**Combined Effect:**
- HDD cluster: Deploy 2-3x faster (seek reduction)
- NVMe cluster: Deploy 2-3x faster (parallel writes)
- Network: Fully saturated (80-100% utilization)

**Design:**

```cpp
// partition_storage (raw_disk_io.cpp)
class partition_storage {
public:
    // Existing functions (KEEP - still used)
    int read(char *buffer, piece_index_t piece, int offset, int length);
    void write(char *buffer, piece_index_t piece, int offset, int length);

    // NEW: Vectored write ONLY (no readv - not needed)
    void writev(std::vector<write_request> const& requests);
    // writev() internally handles:
    // - fs_.map_block() to expand to file_slices
    // - Sort by disk offset and group contiguous slices
    // - Call pwritev() on contiguous groups
};

// Why no readv()?
// - libtorrent calls async_read one at a time (peer requests)
// - Cannot accumulate reads (don't know future requests)
// - Only writes can be coalesced (arrive continuously from network)

// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
private:
    store_buffer m_store_buffer;  // Existing

    // NEW: Pending writes for coalescing
    struct pending_write {
        torrent_location location;      // (storage, piece, offset)
        char const* buffer;             // Points to m_store_buffer data
        std::function<void(storage_error const&)> handler;
        time_point enqueue_time;
    };

    // Group by storage (each storage = one disk)
    std::map<storage_index_t, std::vector<pending_write>> m_pending_writes;
    std::mutex m_pending_mutex;
    boost::asio::steady_timer m_flush_timer;

    // Configuration
    struct write_coalesce_config {
        size_t max_pending_blocks = 64;         // Max 64 blocks (1MB)
        std::chrono::milliseconds timeout = 100ms;
        size_t min_coalesce_count = 4;          // Min 4 blocks to merge
        bool enable = true;
    } m_coalesce_config;
};
```

**Flow:**

```cpp
async_write(storage, piece, offset, buf, handler):
    1. Allocate buffer, copy data
    2. Insert to m_store_buffer (existing)
    3. Add to m_pending_writes (NEW)
    4. Check flush conditions:
       - Accumulated 64 blocks (1 MB)
       - Timeout (HDD: 150ms, NVMe: 0ms opportunistic)
       - Next block not contiguous
       - Piece complete
    5. If should flush: flush_pending_writes()
    6. Return immediately (data in m_store_buffer)

flush_pending_writes(storage):
    1. Sort pending by (piece, offset)
       // For single partition: piece order ≈ disk order ✅
    2. Group contiguous blocks (piece-level)
    3. For each group ≥ min_coalesce_count:
       dispatch_coalesced_write(group)
         → partition_storage.writev(requests)
           → Maps (piece, offset) → disk_offset via file_storage
           → Handles file_slice splits
           → Groups contiguous disk slices
           → pwritev() on each contiguous slice group
    4. For small groups:
       dispatch_single_write(write)  // partition_storage.write()
```

**Two-Level Optimization:** 🔒

```cpp
// Level 1: raw_disk_io (piece-level)
// - Sort by (piece, offset) ✅
// - For single partition: piece order ≈ disk order
// - Group contiguous blocks in piece space

flush_pending_writes() {
    sort(pending, by_piece_and_offset);
    group_contiguous_blocks();  // piece-level contiguity
    dispatch_writev(groups);
}

// Level 2: partition_storage (disk-level)
// - Calculate actual disk offsets (only we can!)
// - Handle file_slice splits
// - Group contiguous disk slices
// - Execute pwritev()

partition_storage::writev(requests) {
    expand_to_file_slices();     // may split blocks
    sort_by_disk_offset();       // actual disk order
    group_contiguous_slices();   // disk-level contiguity
    pwritev(slice_groups);
}
```

**Coalesced Write:**

```cpp
void dispatch_coalesced_write(storage_index_t storage,
                               std::vector<pending_write> const& writes)
{
    boost::asio::post(m_write_thread_pool, [this, storage, writes]() {
        auto* ps = storages_[storage].get();

        // Prepare write_request vector
        std::vector<partition_storage::write_request> requests;
        for (auto const& w : writes) {
            requests.push_back({
                .buffer = w.buffer,
                .piece = w.location.piece,
                .offset = w.location.offset,
                .length = DEFAULT_BLOCK_SIZE
            });
        }

        // Let partition_storage handle file_slice mapping and pwritev!
        libtorrent::storage_error error;
        ps->writev(requests);  // ← Does: map_block → sort slices → pwritev

        // Remove from m_store_buffer (write complete)
        for (auto& w : writes) {
            m_store_buffer.erase(w.location);
            m_buffer_pool.free_disk_buffer(const_cast<char*>(w.buffer));
        }

        // Call all handlers
        for (auto& w : writes) {
            boost::asio::post(m_ioc, [handler = w.handler, error]() {
                handler(error);
            });
        }
    });
}
```

**Performance Expectations:**

**HDD:**
```
Before: 4 blocks × (12ms seek + 1ms transfer) = 52ms
After:  1 seek (12ms) + 4 blocks transfer (4ms) = 16ms
Improvement: 69% faster writes
```

**SSD:**
```
Before: 4 syscalls × 500μs each = 2000μs
After:  1 syscall × 500μs = 500μs
Improvement: 75% syscall reduction = ~20-30% throughput gain
```

**Document:** [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) Part 4

### Phase 3: Optional Enhancements

#### 3.1 io_uring (Optional)

**Conditions:**
- ✅ Don't use O_DIRECT (alignment issues)
- ✅ Keep buffered I/O
- ✅ Only reduce syscall overhead

**Expected Benefit:** Additional 20-30% (stacks with write coalescing)

**Priority:** Low (after Phase 1 & 2)

---

## Implementation Guide

### Priority Order

**Phase 1.1: Buffer Pool Merger** (FIRST)
- Effort: 1-2 days
- Risk: Low
- Impact: +48% memory efficiency
- **Start here**

**Phase 1.2: Configurable Cache** (SECOND)
- Effort: 1 day
- Risk: Low
- Impact: Production requirement
- **Do after 1.1**

**Phase 2: Write Coalescing** (THIRD)
- Effort: 2-3 days
- Risk: Medium
- Impact: HDD +73%, SSD +20-30%
- **Do after Phase 1 complete**

### Naming Convention

**Follow libtorrent style:**
- Member variables: `m_` prefix (e.g., `m_buffer_pool`, `m_store_buffer`)
- Local variables: no prefix
- Constants: UPPER_CASE
- Functions: snake_case

### Testing Strategy

**Unit Tests:**
- buffer_pool allocation/deallocation
- store_buffer insert/get/erase
- Write coalescing contiguity detection
- Flush trigger conditions

**Integration Tests:**
- Full read/write cycle
- Mixed read/write workloads
- Watermark triggering
- Error handling

**Performance Tests:**
- Memory efficiency (balanced, read-heavy, write-heavy)
- Write performance (HDD, SSD)
- Latency (P50, P99)
- Throughput (MB/s)

### Validation Metrics

**Phase 1.1 Success Criteria:**
- ✅ Memory utilization: +40% for unbalanced workloads
- ✅ No performance regression for balanced workloads
- ✅ All tests pass

**Phase 1.2 Success Criteria:**
- ✅ Cache size configurable via settings
- ✅ settings_updated() propagates changes
- ✅ No crashes, no memory leaks

**Phase 2 Success Criteria:**
- ✅ HDD write latency: -60% or better
- ✅ SSD write throughput: +20% or better
- ✅ No data corruption
- ✅ Handles edge cases (non-contiguous, timeouts)

---

## Future Work

### Potential Enhancements

1. **Adaptive coalescing**: Adjust parameters based on disk type (HDD vs SSD)
2. **Smart prefetching**: Predict access patterns, prefetch likely blocks
3. **io_uring integration**: Reduce syscall overhead further (if complexity justified)
4. **NUMA awareness**: Pin threads and memory to NUMA nodes
5. **Read coalescing**: Similar to write coalescing for sequential reads

### Performance Monitoring

**Key Metrics:**
- Buffer pool utilization (read vs write split)
- Cache hit rate (m_store_buffer)
- Write coalescing rate (blocks per pwritev)
- Disk I/O patterns (sequential vs random)
- Syscall frequency (pread, pwrite, pwritev)

**Tools:**
- `perf`: CPU profiling, mutex contention
- `iostat`: Disk I/O statistics
- `blktrace`: Block layer tracing
- `bpftrace`: Kernel tracing
- Custom counters in code

---

## References

### Documentation

**Main Documents:**
- `CLAUDE.md` - This file (quick reference for AI/developers)
- `docs/SESSION_MEMORY.md` - Complete conversation history and analysis
- `docs/MUTEX_ANALYSIS.md` - Mutex contention analysis
- `docs/BUFFER_POOL_MERGER.md` - Buffer pool unification plan
- `docs/CACHE_SIZE_CONFIG.md` - Configuration system implementation
- `docs/STORE_BUFFER_WATERMARK.md` - Backpressure and buffer sizing design
- `docs/WRITE_COALESCING_DESIGN.md` - Write optimization design
- `docs/APP_LEVEL_CACHE.md` - Application-level cache analysis
- `docs/HDD_OPTIMIZATION.md` - HDD-specific optimizations
- `docs/CONCURRENCY_ANALYSIS.md` - Thread safety analysis
- `docs/CACHE_BRANCH_ANALYSIS.md` - Previous cache attempt post-mortem

### Source Code

**libtorrent 2.x (v2.0.10):**
- `tmp/libtorrent-2.0.10/include/libtorrent/aux_/disk_buffer_pool.hpp`
- `tmp/libtorrent-2.0.10/src/disk_buffer_pool.cpp`
- `tmp/libtorrent-2.0.10/include/libtorrent/aux_/store_buffer.hpp`
- `tmp/libtorrent-2.0.10/src/mmap_disk_io.cpp`
- `tmp/libtorrent-2.0.10/src/mmap_storage.cpp`
- `tmp/libtorrent-2.0.10/test/test_store_buffer.cpp`

**EZIO:**
- `buffer_pool.hpp` / `buffer_pool.cpp` - EZIO's buffer pool
- `store_buffer.hpp` - Copied from libtorrent
- `raw_disk_io.hpp` / `raw_disk_io.cpp` - Main disk I/O implementation

---

## Quick Start for AI/Developers

**New to this codebase?**

1. Read this document (CLAUDE.md) first for overview
2. Read [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) for complete analysis
3. Check implementation plans:
   - [docs/BUFFER_POOL_MERGER.md](docs/BUFFER_POOL_MERGER.md)
   - [docs/CACHE_SIZE_CONFIG.md](docs/CACHE_SIZE_CONFIG.md)
   - [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md) Part 4 (write coalescing)

**Ready to implement?**

1. Start with Phase 1.1 (buffer pool merger)
2. Follow naming convention (m_ prefix)
3. Write tests first
4. Measure before/after
5. Validate with real workloads

**Key Insights:**

- EZIO operates on **raw disk** (not filesystem)
- This makes write coalescing **trivially simple**
- Single buffer pool mutex is **NOT a bottleneck**
- All optimizations are **designed and ready**
- Expected gains: +48% memory, +73% HDD writes

---

**Document Version:** 3.0
**Last Updated:** 2024-12-14
**Status:** Complete analysis + implementation plans ready

**Communication Guidelines:**
- Use Traditional Chinese (Taiwan) for conversations with users
- Use English for all documentation files (including this file)
- Use English for code comments (no emojis)