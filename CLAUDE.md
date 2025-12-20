# EZIO Architecture Analysis & Optimization Guide

**Version:** 5.0 (Phase 0, 1.1, 1.2, 3.1 Complete)
**Last Updated:** 2025-12-21
**Reference:** libtorrent-2.0.10 source in `tmp/libtorrent-2.0.10/`
**Complete Memory:** See `docs/SESSION_MEMORY.md` for full conversation history

---

## For New AI Sessions: Quick Context

**If you are a new AI session, please read this section to quickly understand the current state:**

### Current Status (2025-12-21)

**✅ Completed Phases:**
- ✅ **Phase 0: Logging & Debugging** (commits: df30a4a, bccea62)
  - Runtime log level control via environment variables
  - Event-driven alert handling with set_alert_notify()
  - 5000x faster alert response (<1ms vs 0-5s)

- ✅ **Phase 1.1: Buffer Pool Merger** (commit: b018516)
  - Unified buffer pool: 128MB+128MB → 256MB
  - +48% memory efficiency for unbalanced workloads
  - Aligned with libtorrent 2.x design

- ✅ **Phase 1.2: Settings Infrastructure** (commit: c69c69a)
  - Constructor receives settings_interface and counters
  - Thread pools configured from settings (aio_threads, hashing_threads)
  - settings_updated() interface implemented

- ✅ **Phase 3.1: Unified Cache** (commits: 960fdd7 → c18fa72)
  - **Write-through cache** replacing temporary store_buffer
  - 32-way sharded cache with per-partition mutex
  - LRU eviction with dirty block pinning
  - Configurable cache size via `--cache-size` option (default 512MB)
  - Hash-based partition distribution for better load balancing
  - Cache lookup moved to worker threads (main thread optimization)
  - Fixed critical bug: cache_size was 16x too large (32GB → 2GB)
  - Removed unused handler infrastructure (42 lines deleted)

**Key Improvements (2025-12-20 to 2025-12-21):**
- 🐛 **Critical Fix**: cache_size interpreted correctly (16KiB blocks, not KB)
- 🚀 **Performance**: Cache lookup on worker threads, not main thread
- 🎯 **Better Distribution**: Hash-based partitioning (torrent + piece + offset)
- ⚙️  **Configurable**: `--cache-size <MB>` command line option
- 🧹 **Code Quality**: Removed 42 lines (handler cleanup)

**🔥 Next: Phase 2 - Parallel Write Optimization**
- Increase write thread pool for NVMe (simple configuration change)
- Expected: +100-150% write throughput on NVMe

### Critical Architecture Facts

1. **EZIO operates on RAW DISK** (e.g., /dev/sda1), not filesystem!
   - Torrent "files" are disk offset definitions
   - Direct pread()/pwrite() to raw partition
   - No filesystem queries, no fragmentation handling

2. **Unified Buffer Pool** (after Phase 1.1)
   - Single 256MB pool for all operations (read, write, hash)
   - Dynamic allocation with watermarks (50% low, 87.5% high)
   - Fixed size (temporary I/O buffers, not a cache)

3. **Unified Cache** (after Phase 3.1)
   - Write-through cache (default 512MB, configurable via `--cache-size`)
   - 32-way sharded with per-partition mutex for concurrency
   - LRU eviction (dirty blocks pinned during async writes)
   - Replaces temporary store_buffer with persistent read/write cache

4. **Settings Infrastructure** (after Phase 1.2)
   - Constructor: `raw_disk_io(io_context&, settings_interface&, counters&)`
   - Thread pools read from settings in init list
   - Cache size configurable: `./ezio --cache-size 1024` (1GB)

5. **Naming Convention**
   - Member variables use `m_` prefix (libtorrent style)
   - E.g., `m_buffer_pool`, `m_cache`, `m_settings`

### Key Files Navigation

**Main Documents:**
- `CLAUDE.md` - This file, quick reference for new AI sessions
- `docs/SESSION_MEMORY.md` - Complete conversation history
- `docs/PHASE_PLAN_REFORMULATED.md` - Remaining phases (Phase 2+)

**Architecture Documents:**
- `docs/MUTEX_ANALYSIS.md` - Single mutex is NOT a bottleneck (<0.4% utilization)
- `docs/STORE_BUFFER_WATERMARK.md` - Backpressure design (256MB recommendation)
- `docs/WRITE_COALESCING_DESIGN.md` - Write coalescing for Phase 3 (writev only, no readv)

**Completed Phase Documents:**
- `docs/SPDLOG_ADVANCED.md` - Runtime log level control (Phase 0.1)
- `docs/LIBTORRENT_ALERTS_LOGGING.md` - Event-driven alerts (Phase 0.2)
- `docs/BUFFER_POOL_MERGER.md` - Buffer pool unification (Phase 1.1)
- `docs/CACHE_SIZE_CONFIG.md` - Settings infrastructure (Phase 1.2)
- `docs/UNIFIED_CACHE_DESIGN.md` - Unified cache design (Phase 3.1)

**Future Optimization:**
- `docs/FUTURE_OPTIMIZATIONS.md` - 25 optimization opportunities
- `docs/HDD_OPTIMIZATION.md` - HDD-specific strategies
- `docs/APP_LEVEL_CACHE.md` - Application-level cache analysis

### User Requirements

- All documents written in English
- Design documents in `docs/`, except CLAUDE.md in root
- Use libtorrent's `m_` prefix for member variables
- Code comments in English, no emojis
- **Always run clang-format before committing code changes**

**Ready to start implementation? Jump to [Next Steps](#next-steps) section.**

---

## Executive Summary

EZIO is a **BitTorrent-based raw disk imaging tool** for fast LAN deployment. This guide documents the architecture analysis and optimization journey, grounded in libtorrent 2.x source code.

**Critical Discovery: EZIO operates on RAW DISK (e.g., /dev/sda1), not filesystem!**

**Completed Optimizations:**

| Phase | Description | Benefit | Commit Range |
|-------|-------------|---------|--------------|
| 0.1 | Runtime log control | Debug without recompiling | df30a4a |
| 0.2 | Event-driven alerts | 5000x faster response | bccea62 |
| 1.1 | Unified buffer pool | +48% memory efficiency | b018516 |
| 1.2 | Configurable settings | Production tuning | c69c69a |
| 3.1 | Unified cache (write-through) | Read cache + reduced code | 960fdd7→c18fa72 |

**Phase 3.1 Details:**
- Write-through cache replacing store_buffer (512MB default, configurable)
- Fixed critical cache_size bug (was 16x too large!)
- Hash-based partitioning for better load balancing
- Cache lookup on worker threads (main thread optimization)
- Removed 42 lines of unused handler code

**Next Phase (Ready to Implement):**
- **Phase 2**: Parallel write optimization (NVMe +100-150%, just config change!)

---

## Table of Contents

1. [Critical Architecture Discovery](#critical-architecture-discovery)
2. [libtorrent 2.x Architecture](#libtorrent-2x-architecture)
3. [EZIO Implementation](#ezio-implementation)
4. [Key Differences](#key-differences)
5. [Next Steps](#next-steps)
6. [Implementation Guide](#implementation-guide)
7. [References](#references)

---

## Critical Architecture Discovery

### EZIO's True Architecture: Raw Disk

**Most Important Discovery:**

```
EZIO directly reads/writes RAW DISK (e.g., /dev/sda1)
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

**Key Characteristics:**

1. **Single pool**: Read, write, hash ALL share same pool
2. **Dynamic allocation**: Uses malloc()/free() on demand
3. **Watermark**: 50% low, 87.5% high
4. **Short critical sections**: Mutex held for 1-2μs only
5. **No I/O under lock**: Only memory operations

**Mutex Contention Analysis:**

```
16 threads × 100 alloc/sec = 1600 alloc/sec
Total lock time: 1600 × 2μs = 3.2ms/sec
Mutex utilization: 0.32% per second
Availability: 99.68% of time, mutex is FREE

Conclusion: ✅ Single mutex is NOT a bottleneck
```

See [docs/MUTEX_ANALYSIS.md](docs/MUTEX_ANALYSIS.md) for detailed analysis.

### Component 2: store_buffer

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

**Interface:**

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

---

## EZIO Implementation

### Architecture (After Phase 1.1 & 1.2)

**Location:** `raw_disk_io.hpp`

```cpp
class raw_disk_io final : public disk_interface {
private:
    // Unified pool (Phase 1.1)
    buffer_pool m_buffer_pool;  // 256 MB for all operations

    // Temporary cache (copied from libtorrent)
    store_buffer m_store_buffer;

    // Thread pools (configured from settings in Phase 1.2)
    boost::asio::thread_pool read_thread_pool_;
    boost::asio::thread_pool write_thread_pool_;
    boost::asio::thread_pool hash_thread_pool_;

    // Settings infrastructure (Phase 1.2)
    libtorrent::settings_interface const* m_settings;
    libtorrent::counters& m_stats_counters;

    // EZIO uses pread()/pwrite(), not mmap
    // Target: raw disk (/dev/sda1), not filesystem
};
```

**Constructor (After Phase 1.2):**

```cpp
raw_disk_io::raw_disk_io(io_context& ioc,
                          settings_interface const& sett,
                          counters& cnt)
    : ioc_(ioc),
      m_settings(&sett),                                             // Save reference
      m_stats_counters(cnt),                                         // Save reference
      m_buffer_pool(ioc),                                            // Only io_context
      read_thread_pool_(sett.get_int(settings_pack::aio_threads)),  // From settings!
      write_thread_pool_(sett.get_int(settings_pack::aio_threads)), // From settings!
      hash_thread_pool_(sett.get_int(settings_pack::hashing_threads)) // From settings!
{
}

void raw_disk_io::settings_updated() {
    // Reserved for future cache configuration
}
```

---

## Key Differences

### Before vs After Phase 1

**Before (Original EZIO):**
```cpp
// Split pools (42% memory waste)
buffer_pool read_buffer_pool_;   // 128 MB
buffer_pool write_buffer_pool_;  // 128 MB

// Hardcoded thread pools
raw_disk_io(io_context& ioc)
    : read_thread_pool_(8),      // Hardcoded!
      write_thread_pool_(8),     // Hardcoded!
      hash_thread_pool_(8) { }   // Hardcoded!

// Empty settings handler
void settings_updated() { }
```

**After (Phase 1.1 + 1.2):**
```cpp
// Unified pool (+48% efficiency)
buffer_pool m_buffer_pool;  // 256 MB unified

// Configured from settings
raw_disk_io(io_context& ioc, settings_interface const& sett, counters& cnt)
    : m_settings(&sett),
      m_stats_counters(cnt),
      read_thread_pool_(sett.get_int(settings_pack::aio_threads)),   // From settings!
      write_thread_pool_(sett.get_int(settings_pack::aio_threads)),  // From settings!
      hash_thread_pool_(sett.get_int(settings_pack::hashing_threads)) { }

// Implemented settings handler (reserved for future cache)
void settings_updated() {
}
```

**Benefits:**
- ✅ +48% memory efficiency for unbalanced workloads
- ✅ Runtime configuration without recompilation
- ✅ Follows libtorrent 2.x design pattern
- ✅ Foundation ready for Phase 2 optimizations

---

## Next Steps

### Phase 2: Parallel Write Optimization ⚡ **READY**

**What:** Increase write thread pool size to saturate NVMe queue depth

**Why Now:**
- ✅ Settings infrastructure ready (Phase 1.2)
- ✅ Thread pools already configured from settings
- ✅ No code changes needed - just configuration!

**How:**

```cpp
// In your application or session
lt::settings_pack pack;
pack.set_int(lt::settings_pack::aio_threads, 32);  // ← Increase for NVMe!
session.apply_settings(pack);
```

**Expected Results:**
- **NVMe:** +100-150% write throughput (500 MB/s → 1+ GB/s)
- **Saturate 10Gbps network:** Achieve full 1.25 GB/s
- **HDD:** No negative impact (can use lower thread count)

**Effort:** 1 day (mostly testing and tuning)

**See:** [docs/PHASE_PLAN_REFORMULATED.md](docs/PHASE_PLAN_REFORMULATED.md) for details

---

### Future Phases

**Phase 3: Unified Cache + Write Coalescing** (4-6 days)
- Replace store_buffer with persistent sharded cache
- Implement write coalescing with writev()
- Expected: HDD +73%, NVMe +20-30% additional

---

## Implementation Guide

### Code Style

**Before committing any code changes:**

```bash
# Run clang-format on all source files
find . -maxdepth 1 -name "*.cpp" -o -name "*.hpp" | grep -v "./tmp/" | xargs clang-format -i
```

**Important:**
- Always run clang-format before committing
- Ensures consistent code formatting across the project
- Prevents formatting-only commits later

### Testing Strategy

**Unit Tests:**
- buffer_pool allocation/deallocation
- store_buffer insert/get/erase
- Settings propagation

**Integration Tests:**
- Full read/write cycle
- Mixed read/write workloads
- Watermark triggering
- Error handling

**Performance Tests:**
- Memory efficiency (balanced, read-heavy, write-heavy)
- Write performance (HDD, SSD, NVMe)
- Latency (P50, P99)
- Throughput (MB/s)

### Validation Metrics

**Phase 2 Success Criteria:**
- ✅ NVMe write: 500 MB/s → 1+ GB/s (with 32 threads)
- ✅ Network utilization: 80-100%
- ✅ HDD: No regression (with 2-4 threads)
- ✅ Configurable via settings_pack

---

## References

### Documentation

**Main Documents:**
- `CLAUDE.md` - This file (quick reference)
- `docs/SESSION_MEMORY.md` - Complete conversation history
- `docs/PHASE_PLAN_REFORMULATED.md` - Remaining phases
- `docs/MUTEX_ANALYSIS.md` - Mutex contention analysis
- `docs/WRITE_COALESCING_DESIGN.md` - Write optimization design

### Source Code

**libtorrent 2.x (v2.0.10):**
- `tmp/libtorrent-2.0.10/include/libtorrent/aux_/disk_buffer_pool.hpp`
- `tmp/libtorrent-2.0.10/src/disk_buffer_pool.cpp`
- `tmp/libtorrent-2.0.10/src/mmap_disk_io.cpp`

**EZIO:**
- `buffer_pool.hpp` / `buffer_pool.cpp` - Unified buffer pool
- `store_buffer.hpp` - Temporary cache
- `raw_disk_io.hpp` / `raw_disk_io.cpp` - Main disk I/O implementation

---

## Quick Start for AI/Developers

**New to this codebase?**

1. Read this document (CLAUDE.md) for overview
2. Check [docs/PHASE_PLAN_REFORMULATED.md](docs/PHASE_PLAN_REFORMULATED.md) for next tasks
3. Review completed work:
   - Phase 0: Logging (df30a4a, bccea62)
   - Phase 1.1: Buffer pool merger (b018516)
   - Phase 1.2: Settings infrastructure (c69c69a)

**Ready for Phase 2?**

1. Read `/tmp/phase2_new.md` for implementation plan
2. Test different aio_threads values: 2, 8, 16, 32
3. Measure NVMe write throughput
4. Document optimal settings for different storage types

**Key Insights:**

- EZIO operates on **raw disk** (not filesystem)
- Single buffer pool mutex is **NOT a bottleneck**
- Settings infrastructure is **ready for Phase 2**
- Phase 2 is **mostly configuration**, not code

---

**Document Version:** 4.0
**Last Updated:** 2025-12-15
**Status:** Phase 0, 1.1, 1.2 complete - Ready for Phase 2

**Communication Guidelines:**
- Use Traditional Chinese (Taiwan) for conversations with users
- Use English for all documentation files (including this file)
- Use English for code comments (no emojis)
