# Write Coalescing Design Document

**Version:** 1.0
**Date:** 2024-12-14
**Status:** Design Phase

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Motivation](#motivation)
3. [Performance Analysis by Storage Type](#performance-analysis-by-storage-type)
4. [Architecture Design](#architecture-design)
5. [Critical Implementation Details](#critical-implementation-details)
6. [Error Handling](#error-handling)
7. [Testing Strategy](#testing-strategy)
8. [Configuration & Tuning](#configuration--tuning)
9. [Risks & Mitigations](#risks--mitigations)
10. [References](#references)

---

## Executive Summary

**Goal:** Reduce disk write latency by coalescing multiple small writes into larger batched operations using `pwritev()`.

**Key Insight:** EZIO operates on **raw disk**, making write coalescing straightforward.

**Critical Design Decision: ASYMMETRIC - Write Coalescing ONLY**
- ✅ **writev()**: Implement for writes (can accumulate as data arrives)
- ❌ **readv()**: NOT implemented (libtorrent never batches reads)
- Reason: libtorrent calls `async_read` on-demand, one block at a time

**Expected Benefits:**
- **HDD**: +93% throughput (reduce seeks from 64 → 1)
- **SSD (SATA)**: +30-40% throughput (reduce syscall overhead)
- **NVMe SSD**: +20-30% throughput (opportunistic coalescing, no timeout)

**Critical Design Constraint:** Must preserve `store_buffer` semantics - data remains available for reads until disk write completes.

---

## Motivation

### Current Behavior (No Coalescing)

```cpp
// Each 16KB block = separate pwrite() syscall
async_write(piece=5, block=0, 16KB) → pwrite() #1
async_write(piece=5, block=1, 16KB) → pwrite() #2
async_write(piece=5, block=2, 16KB) → pwrite() #3
async_write(piece=5, block=3, 16KB) → pwrite() #4

// HDD cost:
4 blocks × (12ms seek + 1ms transfer) = 52ms total
```

### Proposed Behavior (With Coalescing)

```cpp
// Accumulate contiguous writes
async_write(piece=5, block=0, 16KB) → add to pending_queue
async_write(piece=5, block=1, 16KB) → add to pending_queue
async_write(piece=5, block=2, 16KB) → add to pending_queue
async_write(piece=5, block=3, 16KB) → trigger flush!

// Batch write with pwritev()
flush() → pwritev([block0, block1, block2, block3]) → ONE syscall

// HDD cost:
1 seek (12ms) + 4 blocks transfer (4ms) = 16ms total
Improvement: 69% faster
```

### Why Trivial for EZIO?

**Raw disk** = contiguous address space:

```cpp
// Contiguity check (trivial!):
bool is_contiguous(pending_write const& w1, pending_write const& w2) {
    return w2.disk_offset == w1.disk_offset + DEFAULT_BLOCK_SIZE;
}

// NO need to:
// - Handle file boundaries
// - Query filesystem layout (FIEMAP)
// - Deal with fragmentation
```

---

## Performance Analysis by Storage Type

### HDD (Rotational Disk)

**Characteristics:**
- Seek time: ~12ms (dominant cost)
- Transfer rate: ~150 MB/s
- Random I/O: ~100-200 IOPS
- Sequential I/O: ~150 MB/s

**Coalescing Impact:**

| Scenario | Without Coalescing | With Coalescing | Improvement |
|----------|-------------------|-----------------|-------------|
| 4 blocks (64KB) | 52ms (4 seeks) | 16ms (1 seek) | **69%** |
| 16 blocks (256KB) | 208ms (16 seeks) | 28ms (1 seek) | **87%** |
| 64 blocks (1MB) | 832ms (64 seeks) | 60ms (1 seek) | **93%** |

**Conclusion:** ✅ **CRITICAL optimization for HDD** - seek time dominates.

---

### SSD (SATA)

**Characteristics:**
- Latency: ~100-500μs
- Random I/O: ~80K-100K IOPS
- Sequential I/O: ~500-550 MB/s
- No seek time, but syscall overhead exists

**Coalescing Impact:**

| Scenario | Without Coalescing | With Coalescing | Improvement |
|----------|-------------------|-----------------|-------------|
| 4 blocks (64KB) | 2ms (4 syscalls) | 800μs (1 syscall) | **60%** |
| 16 blocks (256KB) | 8ms (16 syscalls) | 2ms (1 syscall) | **75%** |
| 64 blocks (1MB) | 32ms (64 syscalls) | 6ms (1 syscall) | **81%** |

**Syscall overhead:** ~400-500μs per call

**Conclusion:** ✅ **Significant benefit** - syscall overhead reduction.

---

### NVMe SSD ⚠️ (Updated with Real Deployment Data)

**Theoretical Characteristics:**
- Latency: ~20-100μs
- Random I/O: 300K-1M+ IOPS
- Sequential I/O: 3000-7000 MB/s (high-end models)

**Real-World Clonezilla Deployment Observations:**
- **Network**: 10 Gbps = 1.25 GB/s
- **NVMe Write**: ~4 Gbps = **500 MB/s** (actual measured)
- **Bottleneck**: Disk write, not network! ⚠️

**Why NVMe is Slower Than Spec:**
1. **Mid-range NVMe** (not enterprise Gen4)
2. **Sustained write** (not burst)
3. **Multiple concurrent writes** (10-50 clients)
4. **Thermal throttling** under sustained load
5. **Write amplification** (QD=1 typical for pwrite)

**Critical Discovery:** Even with NVMe, **disk write is the bottleneck** in Clonezilla deployment!

```
Network download: 125-250 MB/s per client (10Gbps / 8-4 clients)
NVMe write:       500 MB/s total
Result:           Writes naturally accumulate in pending queue!
```

**Coalescing Impact (Revised):**

#### Scenario A: Timeout-Based (WRONG Approach)

```
Problem: Artificial delay when writes are already delayed by disk
Network receives block → wait 5ms timeout → flush
Result: Additional latency for no benefit ❌
```

#### Scenario B: Opportunistic Coalescing (CORRECT Approach)

```
Network receives blocks → naturally accumulate in pending queue
                          (because disk is slower than network)
When accumulated 16 blocks OR buffer pressure → flush immediately

No artificial timeout!
Coalescing happens naturally because disk can't keep up.

Result:
- Reduced syscall overhead (16 blocks → 1 pwritev)
- No artificial delay
- Improved throughput ✅
```

**Updated Conclusion:**
- ✅ **Coalescing is BENEFICIAL** even on NVMe
- ❌ **Fixed timeout is HARMFUL**
- ✅ **Use opportunistic strategy**: Only flush when:
  1. Accumulated enough blocks (16+)
  2. Buffer pool pressure (>80%)
  3. Piece complete
  4. **NO fixed timeout** (or very long, like 500ms as safety net)

**Key Insight:** When disk is slower than network, writes **naturally queue up**. Write coalescing simply batches these queued writes more efficiently.

---

### Adaptive Strategy: Storage Type Detection

**Problem:** Need different strategies for different storage types.

**Solution:** Detect storage type and adjust parameters:

```cpp
enum class storage_type {
    HDD,          // Rotational disk
    SSD_SATA,     // SATA SSD
    SSD_NVME,     // NVMe SSD
    UNKNOWN
};

storage_type detect_storage_type(std::string const& device_path) {
    // Method 1: Check rotational flag
    std::string sys_path = "/sys/block/" + device + "/queue/rotational";
    if (read_sys_int(sys_path) == 1) {
        return storage_type::HDD;
    }

    // Method 2: Check if NVMe
    if (device_path.find("/dev/nvme") == 0) {
        return storage_type::SSD_NVME;
    }

    // Method 3: Performance heuristic (measure latency)
    auto latency = measure_write_latency(device_path);
    if (latency < 200us) {
        return storage_type::SSD_NVME;
    } else if (latency < 2ms) {
        return storage_type::SSD_SATA;
    }

    return storage_type::HDD;
}
```

**Configuration by Storage Type:**

```cpp
struct coalesce_config {
    bool enabled;
    size_t max_pending_blocks;      // Max blocks to accumulate
    std::chrono::milliseconds timeout;  // Max wait time
    size_t min_coalesce_count;      // Min blocks to merge
};

coalesce_config get_config(storage_type type) {
    switch (type) {
    case storage_type::HDD:
        return {
            .enabled = true,
            .max_pending_blocks = 64,   // 1 MB
            .timeout = 150ms,           // Wait for more writes
            .min_coalesce_count = 4     // Min 4 blocks (64KB)
        };

    case storage_type::SSD_SATA:
        return {
            .enabled = true,
            .max_pending_blocks = 32,   // 512 KB
            .timeout = 100ms,           // Moderate wait
            .min_coalesce_count = 4
        };

    case storage_type::SSD_NVME:
        return {
            .enabled = true,
            .max_pending_blocks = 64,   // 1 MB (same as HDD!)
            .timeout = 0ms,             // NO TIMEOUT! ⚠️ Opportunistic only
            .min_coalesce_count = 16    // Only batch large groups
        };
        // Rationale: In real deployments (10Gbps network, 4Gbps NVMe),
        // disk write is bottleneck. Writes naturally accumulate without
        // artificial timeout. Only flush when enough blocks accumulate
        // or buffer pressure triggers.

    case storage_type::UNKNOWN:
        return {
            .enabled = true,            // Enable by default
            .max_pending_blocks = 32,
            .timeout = 0ms,             // Opportunistic
            .min_coalesce_count = 8
        };
    }
}
```

**Key Design Principle:**
- **HDD**: Timeout is valuable (avoid seeks)
- **NVMe**: NO timeout (writes already queued by slow disk)

---

## Architecture Design

### Component Overview

```cpp
class raw_disk_io final : public disk_interface {
private:
    // Existing components
    buffer_pool m_buffer_pool;
    store_buffer m_store_buffer;

    // NEW: Write coalescing components

    // Pending write structure
    struct pending_write {
        torrent_location location;      // (storage, piece, offset)
        char const* buffer;             // Points to m_store_buffer data
        uint64_t disk_offset;           // Raw disk offset
        std::function<void(storage_error const&)> handler;
        time_point enqueue_time;        // For timeout tracking
    };

    // Pending writes per storage (one disk = one storage)
    std::map<storage_index_t, std::vector<pending_write>> m_pending_writes;
    std::mutex m_pending_mutex;

    // Flush timer
    boost::asio::steady_timer m_flush_timer;

    // Configuration
    std::map<storage_index_t, storage_type> m_storage_types;
    std::map<storage_index_t, coalesce_config> m_coalesce_configs;

    // Methods
    void enqueue_write(storage_index_t storage, pending_write&& pw);
    void check_flush_conditions(storage_index_t storage);
    void flush_pending_writes(storage_index_t storage);
    void dispatch_coalesced_write(storage_index_t storage,
                                   std::vector<pending_write> const& writes);
    void schedule_flush_timer(storage_index_t storage);
};
```

### Data Flow

```
async_write(storage, piece, offset, buf, handler)
    ↓
1. Allocate buffer, copy data
   m_buffer_pool.allocate_buffer()
   memcpy(buffer, buf, size)
    ↓
2. Insert to store_buffer (data available for reads)
   m_store_buffer.insert({storage, piece, offset}, buffer)
    ↓
3. Calculate disk offset
   disk_offset = piece × piece_size + offset
    ↓
4. Create pending_write
   pending_write pw = {
       location: {storage, piece, offset},
       buffer: buffer,  // ← Points to store_buffer entry
       disk_offset: disk_offset,
       handler: handler,
       enqueue_time: now()
   }
    ↓
5. Enqueue for coalescing
   enqueue_write(storage, std::move(pw))
    ↓
6. Check flush conditions
   if (should_flush()) {
       flush_pending_writes(storage)
   } else {
       schedule_flush_timer(storage)
   }
    ↓
7. Return immediately (caller sees write as complete)


--- Meanwhile, in background ---

flush_pending_writes(storage)
    ↓
1. Sort by disk_offset
2. Group contiguous writes
3. For each group ≥ min_coalesce_count:
   dispatch_coalesced_write(group)  // pwritev
4. For remaining small groups:
   dispatch_single_write(write)      // pwrite
    ↓
dispatch_coalesced_write completes
    ↓
1. pwritev() returns
2. Remove from m_store_buffer
3. Free buffers
4. Call all handlers
```

---

## Critical Implementation Details

### 1. Separation of Concerns: Coalescing Layer vs Storage Layer

**Design Philosophy:** Clean separation between block coalescing logic and file_slice mapping.

#### Current partition_storage Architecture

```cpp
// partition_storage (raw_disk_io.cpp:14-111)
class partition_storage {
    int fd_;
    libtorrent::file_storage const& fs_;

public:
    // Existing functions (KEEP for compatibility)
    int read(char *buffer, piece_index_t piece, int offset, int length);
    void write(char *buffer, piece_index_t piece, int offset, int length);
};

// Current write() implementation:
void write(char *buffer, piece_index_t piece, int offset, int length) {
    // A single block may map to MULTIPLE file_slices
    auto file_slices = fs_.map_block(piece, offset, length);

    for (const auto &file_slice : file_slices) {
        // Parse partition_offset from file_name (e.g., "0x00001000")
        std::string file_name = fs_.file_name(file_slice.file_index);
        int64_t partition_offset = std::stoll(file_name, 0, 16);
        partition_offset += file_slice.offset;

        // Individual pwrite for each slice
        pwrite(fd_, buffer, file_slice.size, partition_offset);
        buffer += file_slice.size;
    }
}
```

#### NEW Design: Add vectored I/O to partition_storage

**Architecture Constraint:** 🔒
- **Only partition_storage has access to `file_storage` (torrent metadata)**
- Only partition_storage can calculate **disk offset** from (piece, offset)
- All disk offset calculations MUST happen inside partition_storage

**Better Separation of Concerns:**

1. **raw_disk_io (coalescing layer)**:
   - Works with **torrent coordinates** (piece, offset)
   - Groups requests and decides when to flush
   - Knows NOTHING about disk offsets or file_slices

2. **partition_storage (storage layer)**:
   - Receives batch of requests in torrent coordinates
   - Calculates disk offsets via file_storage
   - Sorts by disk offset and groups contiguous writes
   - Executes pwritev()

```cpp
// NEW: Add writev() to partition_storage
class partition_storage {
    int fd_;
    libtorrent::file_storage const& fs_;

public:
    // Existing functions (KEEP)
    int read(char *buffer, piece_index_t piece, int offset, int length);
    void write(char *buffer, piece_index_t piece, int offset, int length);

    // NEW: Vectored write (PRIMARY optimization target)
    void writev(std::vector<write_request> const& requests,
                libtorrent::storage_error& error);

    // Note: readv() NOT implemented - libtorrent never batches reads
    // async_read is always single-block (peer requests specific block)
};

// Request structure
struct write_request {
    char const* buffer;         // Source buffer
    piece_index_t piece;
    int offset;
    int length;
};
```

#### partition_storage::writev() Implementation

**Critical:** This function does ALL the heavy lifting because only partition_storage has access to `file_storage`.

```cpp
void partition_storage::writev(std::vector<write_request> const& requests,
                                libtorrent::storage_error& error) {
    // Fast path: single request → use existing write()
    if (requests.size() == 1) {
        write(const_cast<char*>(requests[0].buffer),
              requests[0].piece,
              requests[0].offset,
              requests[0].length,
              error);
        return;
    }

    // Multiple requests: optimize with pwritev
    // Step 1: Expand all requests into file_slices with DISK OFFSETS
    // ⚠️ ONLY partition_storage can do this! (has fs_)
    struct slice_info {
        int64_t disk_offset;        // ← Calculated from file_storage
        char const* buffer;
        size_t size;
    };
    std::vector<slice_info> all_slices;

    for (auto const& req : requests) {
        // Use file_storage to map (piece, offset) → file_slices
        auto file_slices = fs_.map_block(req.piece, req.offset, req.length);

        char const* buf_ptr = req.buffer;
        for (auto const& fs : file_slices) {
            // Calculate disk offset (ONLY possible here!)
            std::string file_name(fs_.file_name(fs.file_index));
            int64_t disk_offset = 0;

            try {
                disk_offset = std::stoll(file_name, 0, 16);
                disk_offset += fs.offset;
            } catch (const std::exception& e) {
                error.file(fs.file_index);
                error.ec = libtorrent::errors::parse_failed;
                error.operation = libtorrent::operation_t::file_write;
                return;
            }

            all_slices.push_back({
                .disk_offset = disk_offset,
                .buffer = buf_ptr,
                .size = fs.size
            });

            buf_ptr += fs.size;
        }
    }

    // Step 2: Sort by disk_offset (CRITICAL for coalescing!)
    // ⚠️ raw_disk_io cannot do this - it doesn't know disk offsets
    std::sort(all_slices.begin(), all_slices.end(),
        [](auto const& a, auto const& b) {
            return a.disk_offset < b.disk_offset;
        });

    // Step 3: Group contiguous slices
    std::vector<std::vector<slice_info>> groups;
    std::vector<slice_info> current_group;
    current_group.push_back(all_slices[0]);

    for (size_t i = 1; i < all_slices.size(); ++i) {
        auto const& last = current_group.back();
        auto const& curr = all_slices[i];

        // Check if disk offsets are contiguous
        if (curr.disk_offset == last.disk_offset + (int64_t)last.size) {
            // Contiguous!
            current_group.push_back(curr);
        } else {
            // Gap - flush current group
            groups.push_back(std::move(current_group));
            current_group.clear();
            current_group.push_back(curr);
        }
    }
    groups.push_back(std::move(current_group));

    // Step 4: Write each contiguous group
    for (auto const& group : groups) {
        ssize_t written;

        if (group.size() == 1) {
            // Single slice - use pwrite
            written = pwrite(fd_, group[0].buffer, group[0].size,
                            group[0].disk_offset);
            if (written != (ssize_t)group[0].size) {
                error.ec = libtorrent::errors::file_write;
                error.operation = libtorrent::operation_t::file_write;
                return;
            }
        } else {
            // Multiple contiguous slices - use pwritev
            std::vector<iovec> iov(group.size());
            ssize_t expected = 0;

            for (size_t i = 0; i < group.size(); ++i) {
                iov[i].iov_base = const_cast<char*>(group[i].buffer);
                iov[i].iov_len = group[i].size;
                expected += group[i].size;
            }

            written = pwritev(fd_, iov.data(), iov.size(),
                             group[0].disk_offset);
            if (written != expected) {
                error.ec = libtorrent::errors::file_write;
                error.operation = libtorrent::operation_t::file_write;
                return;
            }
        }
    }
}

// Note: readv() is NOT implemented
// Reason: libtorrent NEVER batches reads
// - async_read is always on-demand (peer requests specific block)
// - No benefit from implementing readv() since it will never be called
```

#### Simplified Coalescing Layer (raw_disk_io)

**Design Decision:** For single partition case, piece + offset order ≈ disk order, so raw_disk_io CAN sort.

```cpp
// raw_disk_io: Sort by (piece, offset) and group
void flush_pending_writes(storage_index_t storage) {
    std::unique_lock<std::mutex> l(m_pending_mutex);
    auto& pending = m_pending_writes[storage];
    if (pending.empty()) return;

    auto const& config = m_coalesce_configs[storage];

    // Step 1: Sort by (piece, offset)
    // For single partition: this gives disk-sequential order ✅
    std::sort(pending.begin(), pending.end(),
        [](auto const& a, auto const& b) {
            if (a.location.piece != b.location.piece)
                return a.location.piece < b.location.piece;
            return a.location.offset < b.location.offset;
        });

    // Step 2: Group contiguous blocks (in piece space)
    std::vector<std::vector<pending_write>> groups;
    std::vector<pending_write> current_group;
    current_group.push_back(std::move(pending[0]));

    for (size_t i = 1; i < pending.size(); ++i) {
        auto const& last = current_group.back();
        auto& curr = pending[i];

        // Check piece-level contiguity
        bool contiguous =
            (curr.location.piece == last.location.piece) &&
            (curr.location.offset == last.location.offset + DEFAULT_BLOCK_SIZE);

        if (contiguous) {
            current_group.push_back(std::move(curr));
        } else {
            groups.push_back(std::move(current_group));
            current_group.clear();
            current_group.push_back(std::move(curr));
        }
    }
    groups.push_back(std::move(current_group));

    pending.clear();
    l.unlock();

    // Step 3: Dispatch groups
    for (auto& group : groups) {
        if (group.size() >= config.min_coalesce_count) {
            // Batch write via partition_storage.writev()
            dispatch_coalesced_write(storage, group);
        } else {
            // Too small - write individually
            for (auto& w : group) {
                dispatch_single_write(storage, w);
            }
        }
    }
}

// Note: partition_storage.writev() will:
// 1. Calculate actual disk offsets
// 2. Handle file_slice splits
// 3. Group contiguous slices
// 4. Execute pwritev()
//
// So even if raw_disk_io groups by piece-contiguity,
// partition_storage ensures correctness at disk level.

void dispatch_coalesced_write(
    storage_index_t storage,
    std::vector<pending_write> const& writes) {

    // Prepare write_request vector for partition_storage
    std::vector<partition_storage::write_request> requests;
    requests.reserve(writes.size());

    for (auto const& w : writes) {
        requests.push_back({
            .buffer = w.buffer,
            .piece = w.location.piece,
            .offset = w.location.offset,
            .length = DEFAULT_BLOCK_SIZE
        });
    }

    // Call partition_storage::writev()
    // It handles file_slice mapping and pwritev internally
    boost::asio::post(m_write_thread_pool, [this, storage, writes, requests]() {
        auto* ps = storages_[storage].get();

        libtorrent::storage_error error;
        ps->writev(requests);  // ← partition_storage handles complexity!

        // Clean up store_buffer and notify handlers
        for (auto const& w : writes) {
            m_store_buffer.erase(w.location);
            m_buffer_pool.free_disk_buffer(const_cast<char*>(w.buffer));

            boost::asio::post(m_ioc, [handler = w.handler, error]() {
                handler(error);
            });
        }
    });
}
```

#### Benefits of This Design

**Separation of Concerns:**
```
raw_disk_io:
  - ✅ Works with torrent coordinates (piece, offset)
  - ✅ Decides WHEN to flush (timeout, count, pressure)
  - ✅ Accumulates writes and batches them
  - ❌ CANNOT sort by disk offset (doesn't have file_storage)
  - ❌ CANNOT know if writes are disk-contiguous

partition_storage:
  - ✅ Has file_storage access (torrent metadata)
  - ✅ Calculates disk offsets from torrent coordinates
  - ✅ Sorts by disk offset for optimal coalescing
  - ✅ Groups contiguous disk writes
  - ✅ Executes pwritev() on contiguous groups
  - ✅ Single responsibility: disk layout mapping
```

**Important Clarification:**
```
For EZIO's raw disk case (single partition):
  piece + offset order ≈ disk offset order ✅

Example (single partition):
  piece 0, offset 0     → disk offset 0x0
  piece 0, offset 16KB  → disk offset 0x4000
  piece 1, offset 0     → disk offset 0x100000  (piece_size = 1MB)
  piece 1, offset 16KB  → disk offset 0x104000

Sorting by (piece, offset):
  piece 0, offset 0 → piece 0, offset 16KB → piece 1, offset 0 → ...
  ✅ Same order as disk offsets!

HOWEVER: partition_storage still needs to:
  1. Calculate ACTUAL disk offset values (not just order)
  2. Handle file_slices (block may span multiple slices)
  3. Detect gaps between non-contiguous file_slices

So: raw_disk_io CAN sort by (piece, offset) for ordering
    partition_storage MUST calculate disk offsets for actual I/O
```

**Edge Case: Multiple partitions (complex file_storage)**
```
// Rare case: torrent with multiple partitions
piece 0 → partition A (offset 0x0)
piece 1 → partition B (offset 0x0)  ← Different disk!

In this case:
  - piece order ≠ disk offset order
  - partition_storage groups by fd_ automatically
  - Each fd_ writes independently
```

**Backward Compatibility:**
```cpp
// Existing functions (KEEP - still used!)
partition_storage->read(buffer, piece, offset, length);   // All reads use this
partition_storage->write(buffer, piece, offset, length);  // Single writes use this

// NEW function (write coalescing ONLY)
partition_storage->writev(requests);  // Batched writes use this

// Implementation: writev() delegates to write() for single request
void partition_storage::writev(std::vector<write_request> const& requests,
                                libtorrent::storage_error& error) {
    if (requests.size() == 1) {
        // Fast path: just call existing write()
        write(const_cast<char*>(requests[0].buffer),
              requests[0].piece,
              requests[0].offset,
              requests[0].length,
              error);
        return;
    }

    // Multiple requests: do full optimization
    // ... (sorting, grouping, pwritev) ...
}
```

**Usage Pattern:**
```cpp
// ALL reads: direct call
async_read() → partition_storage->read()

// Single write: direct call (rare - timeout case)
async_write() → partition_storage->write()

// Batched writes: via writev (common - coalesced)
flush_pending_writes() → partition_storage->writev()
```

**Asymmetric Design: Write Coalescing ONLY**

**Key Insight:** Only writes benefit from coalescing, reads cannot be batched.

```cpp
// async_read: ALWAYS single block
// - libtorrent calls async_read for each peer request
// - Each peer requests specific (piece, offset)
// - CANNOT accumulate multiple reads (don't know future requests)
// - CANNOT read-ahead (don't know which pieces are verified)

async_read(storage, piece, offset, handler) {
    // Direct read - no coalescing possible
    partition_storage->read(buffer, piece, offset, length);
}

// async_write: CAN accumulate → use writev()
// - Receives blocks from network as they arrive
// - Can delay writes (data in m_store_buffer)
// - Accumulate multiple writes in m_pending_writes
// - Flush as batch when threshold reached

async_write(storage, piece, offset, handler) {
    // Step 1: Store in m_store_buffer (for async_read)
    m_store_buffer.insert(location, buffer);

    // Step 2: Enqueue for coalescing
    enqueue_write(...);  // Add to m_pending_writes

    // Step 3: Check if should flush
    if (should_flush) {
        flush_pending_writes();
          → partition_storage->writev(pending_requests);  // Batched!
    }

    // Return immediately (write happens in background)
}
```

**Why Asymmetric?**

| Operation | Can Batch? | Reason |
|-----------|-----------|---------|
| **Read** | ❌ No | libtorrent calls on-demand, one at a time |
| **Write** | ✅ Yes | Data arrives continuously, can accumulate |

**Result:** Only implement `writev()`, not `readv()`.
```

**Complexity Analysis:**
```
raw_disk_io layer:
  - O(N log N) sort pending writes
  - O(N) group contiguous blocks
  Cost: Minimal, happens in background

partition_storage layer:
  - O(N) expand to file_slices
  - O(N log N) sort slices
  - O(N) group and write
  Cost: Acceptable, only when flushing
```

### 2. Flush Trigger Conditions (Opportunistic Strategy)

```cpp
void check_flush_conditions(storage_index_t storage) {
    auto const& config = m_coalesce_configs[storage];
    auto& pending = m_pending_writes[storage];

    bool should_flush = false;

    // Condition 1: Accumulated enough blocks (PRIMARY trigger)
    if (pending.size() >= config.max_pending_blocks) {
        should_flush = true;
    }

    // Condition 2: Buffer pool pressure (CRITICAL trigger)
    size_t buffer_usage = m_buffer_pool.usage_percentage();
    if (buffer_usage > 80%) {
        should_flush = true;  // Free buffers ASAP
    }

    // Condition 3: Accumulated minimum batch size
    if (pending.size() >= config.min_coalesce_count) {
        // Have enough for efficient batching
        should_flush = true;
    }

    // Condition 4: Timeout (ONLY if configured)
    if (config.timeout > std::chrono::milliseconds(0) && !pending.empty()) {
        auto oldest = pending.front().enqueue_time;
        auto age = std::chrono::steady_clock::now() - oldest;
        if (age > config.timeout) {
            should_flush = true;
        }
    }

    // Condition 5: Piece complete (flush to free store_buffer)
    // (Checked by caller - async_write knows when piece completes)

    if (should_flush) {
        flush_pending_writes(storage);
    } else if (config.timeout > std::chrono::milliseconds(0) &&
               !pending.empty()) {
        // Only schedule timer if timeout is configured
        schedule_flush_timer(storage);
    }
    // else: opportunistic mode - no timer, only flush on accumulation
}
```

**Opportunistic Mode (timeout=0ms):**
```
NVMe deployment scenario:
1. Network downloads blocks faster than disk can write
2. Blocks accumulate in pending queue naturally
3. When 16 blocks accumulated → flush (Condition 1 or 3)
4. No artificial waiting!

Result:
- Writes batch efficiently without timeout delay
- Throughput limited by disk (500 MB/s), not syscall overhead
- Best of both worlds: batching + no latency penalty
```

### 3. Adaptive Timeout Based on Buffer Pressure

```cpp
std::chrono::milliseconds get_adaptive_timeout(
    storage_index_t storage) {

    auto const& base_config = m_coalesce_configs[storage];
    size_t buffer_usage = m_buffer_pool.usage_percentage();
    size_t pending_count = m_pending_writes[storage].size();

    // High buffer pressure - reduce timeout
    if (buffer_usage > 85% || pending_count > base_config.max_pending_blocks * 0.8) {
        return base_config.timeout / 5;  // Very aggressive
    } else if (buffer_usage > 70%) {
        return base_config.timeout / 2;  // Moderately aggressive
    }

    // Normal - use configured timeout
    return base_config.timeout;
}

void schedule_flush_timer(storage_index_t storage) {
    auto timeout = get_adaptive_timeout(storage);

    m_flush_timer.expires_after(timeout);
    m_flush_timer.async_wait([this, storage](boost::system::error_code const& ec) {
        if (!ec) {
            flush_pending_writes(storage);
        }
    });
}
```

### 4. store_buffer Lifetime Management

**Critical Constraint:** Buffer pointer in `pending_write` must remain valid until write completes.

**Guaranteed by Design:**
```cpp
// When write is enqueued:
async_write() {
    char* buf = m_buffer_pool.allocate_buffer();
    memcpy(buf, data, size);

    m_store_buffer.insert(location, buf);  // ← Inserted

    pending_write pw;
    pw.buffer = buf;  // ← Points to store_buffer entry

    enqueue_write(storage, std::move(pw));
    // Buffer still in store_buffer, pointer valid
}

// When write completes:
dispatch_coalesced_write() {
    pwritev(fd, iov, count, offset);  // ← Disk write

    // Only NOW remove from store_buffer
    for (auto& w : writes) {
        m_store_buffer.erase(w.location);  // ← Removed
        m_buffer_pool.free_disk_buffer(w.buffer);
    }
}

// Meanwhile, async_read can still access:
async_read() {
    bool found = m_store_buffer.get(location, [&](char const* buf) {
        memcpy(read_buffer, buf, size);  // ← Works! Still in store_buffer
    });
}
```

**No race condition because:**
1. `store_buffer.insert()` happens **before** enqueueing
2. `store_buffer.erase()` happens **after** disk write completes
3. Buffer pointer remains valid during entire pending period
4. `async_read()` holds `store_buffer` mutex during access

---

## Error Handling

### pwritev() Partial Success

**Problem:** `pwritev()` can write fewer bytes than requested.

```cpp
void dispatch_coalesced_write(
    storage_index_t storage,
    std::vector<pending_write> const& writes) {

    int fd = m_storages[storage]->get_disk_fd();

    // Prepare iovec
    std::vector<iovec> iov(writes.size());
    for (size_t i = 0; i < writes.size(); ++i) {
        iov[i].iov_base = const_cast<char*>(writes[i].buffer);
        iov[i].iov_len = DEFAULT_BLOCK_SIZE;
    }

    uint64_t offset = writes[0].disk_offset;
    size_t expected = writes.size() * DEFAULT_BLOCK_SIZE;
    ssize_t written = pwritev(fd, iov.data(), iov.size(), offset);

    if (written == (ssize_t)expected) {
        // ✅ Complete success
        handle_write_success(writes);
    } else if (written > 0) {
        // ⚠️ Partial success
        size_t succeeded_blocks = written / DEFAULT_BLOCK_SIZE;

        // Succeeded writes
        std::vector<pending_write> succeeded(
            writes.begin(),
            writes.begin() + succeeded_blocks
        );
        handle_write_success(succeeded);

        // Failed writes - retry as individual writes
        for (size_t i = succeeded_blocks; i < writes.size(); ++i) {
            dispatch_single_write(storage, writes[i]);
        }
    } else {
        // ❌ Complete failure
        storage_error error;
        error.ec = make_error_code(
            boost::system::errc::errc_t(errno));
        error.operation = operation_t::file_write;

        handle_write_failure(writes, error);
    }
}

void handle_write_success(std::vector<pending_write> const& writes) {
    for (auto& w : writes) {
        // Remove from store_buffer
        m_store_buffer.erase(w.location);

        // Free buffer
        m_buffer_pool.free_disk_buffer(const_cast<char*>(w.buffer));

        // Notify success
        boost::asio::post(m_ioc, [handler = w.handler]() {
            handler(storage_error{});
        });
    }
}

void handle_write_failure(
    std::vector<pending_write> const& writes,
    storage_error const& error) {

    for (auto& w : writes) {
        // Clean up
        m_store_buffer.erase(w.location);
        m_buffer_pool.free_disk_buffer(const_cast<char*>(w.buffer));

        // Notify failure
        boost::asio::post(m_ioc, [handler = w.handler, error]() {
            handler(error);
        });
    }
}
```

### ENOSPC (Disk Full)

```cpp
// When disk is full, pwritev() returns ENOSPC
// Strategy: Fail all pending writes immediately, don't retry

if (written < 0 && errno == ENOSPC) {
    storage_error error;
    error.ec = make_error_code(boost::system::errc::no_space_on_device);
    error.operation = operation_t::file_write;

    // Fail all pending writes for this storage
    flush_all_pending_with_error(storage, error);
}
```

### Timeout without Natural Batching (NVMe)

```cpp
// NVMe with short timeout: if only 1-2 writes pending, don't batch
void flush_pending_writes(storage_index_t storage) {
    auto& pending = m_pending_writes[storage];
    auto const& config = m_coalesce_configs[storage];

    // NVMe optimization: Don't batch tiny groups
    if (m_storage_types[storage] == storage_type::SSD_NVME &&
        pending.size() < config.min_coalesce_count) {
        // Write individually - no benefit from batching
        for (auto& w : pending) {
            dispatch_single_write(storage, w);
        }
        pending.clear();
        return;
    }

    // Otherwise, proceed with normal grouping logic
    // ...
}
```

---

## Testing Strategy

### Unit Tests

**Test 1: Contiguity Detection**
```cpp
TEST(write_coalescing, contiguity_detection) {
    std::vector<pending_write> writes = {
        {.disk_offset = 0x1000},
        {.disk_offset = 0x1000 + 16KB},  // Contiguous
        {.disk_offset = 0x1000 + 32KB},  // Contiguous
        {.disk_offset = 0x2000},         // Gap!
        {.disk_offset = 0x2000 + 16KB},  // Contiguous with 0x2000
    };

    auto groups = group_contiguous_writes(writes);

    EXPECT_EQ(groups.size(), 2);
    EXPECT_EQ(groups[0].size(), 3);  // First 3 are contiguous
    EXPECT_EQ(groups[1].size(), 2);  // Last 2 are contiguous
}
```

**Test 2: Partial pwritev Success**
```cpp
TEST(write_coalescing, partial_success) {
    // Mock pwritev to return 32KB (2 blocks) instead of 64KB (4 blocks)
    mock_pwritev_return_value(32 * 1024);

    std::vector<pending_write> writes = create_test_writes(4);
    dispatch_coalesced_write(storage, writes);

    // Verify:
    // - First 2 writes: success callback called
    // - Last 2 writes: retried with individual pwrite
    EXPECT_EQ(success_count, 2);
    EXPECT_EQ(retry_count, 2);
}
```

**Test 3: Timeout Trigger**
```cpp
TEST(write_coalescing, timeout_trigger) {
    coalesce_config config{
        .timeout = 50ms,
        .min_coalesce_count = 4
    };

    // Enqueue 2 writes (below min_coalesce_count)
    enqueue_write(storage, write1);
    enqueue_write(storage, write2);

    // Timer should be scheduled
    EXPECT_TRUE(timer_is_active());

    // After 50ms, flush should trigger even with only 2 writes
    advance_time(50ms);

    EXPECT_EQ(flushed_count, 2);
}
```

**Test 4: Buffer Pressure Flush**
```cpp
TEST(write_coalescing, buffer_pressure_flush) {
    // Fill buffer pool to 85%
    fill_buffer_pool(85);

    // Enqueue 1 write
    enqueue_write(storage, write1);

    // Should flush immediately despite timeout not reached
    EXPECT_EQ(flushed_count, 1);
}
```

### Integration Tests

**Test 5: Full Write Cycle with Coalescing**
```cpp
TEST(integration, write_coalescing_full_cycle) {
    // Setup: 50GB torrent, HDD storage

    // Simulate BitTorrent piece downloads
    for (int piece = 0; piece < 100; ++piece) {
        for (int block = 0; block < 4; ++block) {
            async_write(storage, piece, block * 16KB, data, handler);
        }

        // Verify coalescing happened
        EXPECT_EQ(pwritev_calls, piece + 1);  // One pwritev per piece
        EXPECT_GT(pwrite_calls, 0);  // Individual writes for stragglers
    }
}
```

**Test 6: Mixed Read/Write During Coalescing**
```cpp
TEST(integration, read_during_pending_write) {
    // Write piece 5, block 0 (enters pending queue)
    async_write(storage, piece=5, block=0, data, write_handler);

    // Immediately read piece 5, block 0 (before flush)
    async_read(storage, piece=5, block=0, read_handler);

    // Verify:
    // 1. Read succeeds (from store_buffer)
    // 2. Data matches written data
    // 3. No disk I/O for read (cache hit)
    EXPECT_TRUE(read_succeeded);
    EXPECT_EQ(memcmp(read_data, write_data, 16KB), 0);
    EXPECT_EQ(disk_read_count, 0);  // Read from store_buffer
}
```

### Performance Tests

**Test 7: HDD Write Latency**
```cpp
TEST(performance, hdd_write_latency) {
    // Setup: Real HDD, 1MB sequential write

    auto start = steady_clock::now();

    for (int i = 0; i < 64; ++i) {
        async_write(storage, piece=0, offset=i*16KB, data, handler);
    }

    wait_for_all_writes();
    auto duration = steady_clock::now() - start;

    // Without coalescing: ~832ms (64 seeks)
    // With coalescing: ~60ms (1 seek + transfers)
    EXPECT_LT(duration, 100ms);  // Should be much faster
}
```

**Test 8: NVMe Write Latency (No Artificial Delay)**
```cpp
TEST(performance, nvme_write_latency) {
    // Setup: Real NVMe, timeout=5ms

    auto start = steady_clock::now();

    // Single write
    async_write(storage, piece=0, offset=0, data, handler);

    wait_for_write();
    auto duration = steady_clock::now() - start;

    // Should complete in ~50-100μs, NOT delayed by timeout
    EXPECT_LT(duration, 1ms);  // Very fast, no artificial delay
}
```

---

## Configuration & Tuning

### Recommended Settings by Storage Type (Updated for Real Deployments)

```cpp
// HDD (Aggressive Coalescing with Timeout)
{
    .enabled = true,
    .max_pending_blocks = 64,        // 1 MB accumulation
    .timeout = 150ms,                // Wait for more writes (reduces seeks)
    .min_coalesce_count = 4          // Min 64 KB batch
}

// SATA SSD (Moderate Coalescing)
{
    .enabled = true,
    .max_pending_blocks = 32,        // 512 KB accumulation
    .timeout = 100ms,                // Moderate wait
    .min_coalesce_count = 4
}

// NVMe SSD (Opportunistic Coalescing - NO TIMEOUT)
{
    .enabled = true,
    .max_pending_blocks = 64,        // 1 MB accumulation (same as HDD!)
    .timeout = 0ms,                  // NO TIMEOUT! ⚠️
    .min_coalesce_count = 16         // Only batch large groups
}
// Rationale: Real NVMe deployments (10Gbps network, 4Gbps disk)
// show disk is bottleneck. Writes naturally accumulate without
// artificial delay. Coalescing provides syscall reduction benefit
// without latency penalty.
```

**Performance Expectations (Updated):**

```
HDD (12ms seek, 150 MB/s):
  Without coalescing: 64 blocks × 13ms = 832ms
  With coalescing:    1 pwritev = 60ms
  Improvement: 93% ✅

SATA SSD (500μs latency, 500 MB/s):
  Without coalescing: 64 syscalls × ~500μs = 32ms
  With coalescing:    1 syscall = 2ms
  Improvement: 94% ✅

NVMe (4Gbps = 500 MB/s sustained in real deployment):
  Network: 125-250 MB/s download per client
  Disk:    500 MB/s write (shared among clients)
  Result:  Writes queue up naturally

  Without coalescing: 64 syscalls = overhead + queue time
  With coalescing:    1 syscall = same queue time, less overhead
  Improvement: 20-30% throughput gain ✅

  Key: NO artificial delay because disk is already slower than network!
```

### Runtime Configuration (settings_pack)

```cpp
// Add to libtorrent settings_pack
namespace settings_pack {
    // Write coalescing settings
    bool_type_base const enable_write_coalescing = 0x1000;
    int_type_base const write_coalesce_max_blocks = 0x1001;
    int_type_base const write_coalesce_timeout_ms = 0x1002;
    int_type_base const write_coalesce_min_count = 0x1003;
}

// Usage in EZIO
void raw_disk_io::settings_updated() {
    m_buffer_pool.set_settings(m_settings);

    // Update coalescing config
    for (auto& [storage, config] : m_coalesce_configs) {
        config.enabled = m_settings.get_bool(
            settings_pack::enable_write_coalescing);
        config.max_pending_blocks = m_settings.get_int(
            settings_pack::write_coalesce_max_blocks);
        config.timeout = std::chrono::milliseconds(
            m_settings.get_int(settings_pack::write_coalesce_timeout_ms));
        config.min_coalesce_count = m_settings.get_int(
            settings_pack::write_coalesce_min_count);
    }
}
```

### Auto-Tuning (Advanced)

```cpp
// Measure actual performance and adjust
struct performance_stats {
    double avg_write_latency_ms;
    size_t coalesced_writes;
    size_t individual_writes;
    double coalesce_ratio;  // coalesced / (coalesced + individual)
};

void auto_tune_coalescing(storage_index_t storage) {
    auto stats = collect_stats(storage);
    auto& config = m_coalesce_configs[storage];

    // If coalesce ratio is low, timeout might be too short
    if (stats.coalesce_ratio < 0.5) {
        config.timeout *= 1.5;  // Increase timeout
    }

    // If latency is high, might be waiting too long
    if (stats.avg_write_latency_ms > 100) {
        config.timeout *= 0.8;  // Decrease timeout
    }

    // Clamp to reasonable range
    config.timeout = std::clamp(config.timeout, 5ms, 200ms);
}
```

---

## Risks & Mitigations

### Risk 1: Increased Latency on NVMe ⚠️

**Risk:** Artificial delay (timeout) increases latency unnecessarily.

**Mitigation:**
- ✅ Detect NVMe and use very short timeout (5ms)
- ✅ Only batch if `min_coalesce_count` naturally arrives
- ✅ Buffer pressure triggers immediate flush
- ✅ Measure P99 latency in testing

**Acceptance Criteria:** P99 latency should not increase by more than 10% on NVMe.

---

### Risk 2: Buffer Pool Exhaustion

**Risk:** Pending writes hold buffers, potentially exhausting pool.

**Mitigation:**
- ✅ Track buffer usage percentage
- ✅ Force flush when usage > 80%
- ✅ Limit `max_pending_blocks` to 25% of total pool size
- ✅ Adaptive timeout based on buffer pressure

**Acceptance Criteria:** Buffer pool should never reach 100% due to pending writes.

---

### Risk 3: pwritev() Partial Success Complexity

**Risk:** Handling partial writes adds complexity and potential bugs.

**Mitigation:**
- ✅ Comprehensive unit tests for all cases
- ✅ Retry failed portion with individual `pwrite()`
- ✅ Detailed logging of partial failures
- ✅ Fail gracefully without data loss

**Acceptance Criteria:** All edge cases handled correctly in tests.

---

### Risk 4: Incorrect Contiguity Detection

**Risk:** Bug in contiguity check causes non-contiguous writes to be merged.

**Mitigation:**
- ✅ Simple comparison: `offset2 == offset1 + block_size`
- ✅ Extensive unit tests
- ✅ Verify with real disk I/O pattern analysis (blktrace)
- ✅ Assert invariants in debug builds

**Acceptance Criteria:** blktrace shows correct sequential write patterns.

---

## References

### Related Documents

- `docs/BUFFER_POOL_MERGER.md` - Buffer pool unification (dependency)
- `docs/CACHE_SIZE_CONFIG.md` - Configuration system (dependency)
- `docs/SESSION_MEMORY.md` - Complete analysis history
- `docs/MUTEX_ANALYSIS.md` - Mutex contention analysis

### System Calls

- `pwritev(2)` - Vectored write (gather write)
- `pwrite(2)` - Positional write
- `fsync(2)` - Flush data to disk (not used during normal operation)

### Linux Kernel Documentation

- `/sys/block/*/queue/rotational` - Storage type detection
- `/sys/block/*/queue/scheduler` - I/O scheduler type
- `blktrace(8)` - Block layer I/O tracing

### libtorrent API

- `disk_interface::async_write()` - Main write entry point
- `store_buffer` - Temporary cache component
- `buffer_allocator_interface` - Buffer management interface

---

## Advanced: Saturating 10Gbps Network with NVMe

### Current Bottleneck Analysis

**Target:** 10 Gbps = 1.25 GB/s network utilization

**Current Reality:**
```
NVMe sustained write: 500 MB/s (4 Gbps)
Network capacity:     1.25 GB/s (10 Gbps)
Bottleneck:           NVMe disk write ❌
Utilization:          40% of network capacity
```

**Why NVMe is Underperforming:**

```cpp
// Current implementation (synchronous)
for (each write group) {
    pwritev(fd, iov, count, offset);  // ← Blocks until complete
    // Next write waits...
}

Queue Depth: 1 (serial writes)
Result: NVMe idle most of the time, waiting for syscall overhead
```

### Solution: Parallel Asynchronous Writes

#### Problem: Serial Writes (QD=1)

```
Timeline with serial pwritev:
┌────────┐         ┌────────┐         ┌────────┐
│Write 1 │ (wait)  │Write 2 │ (wait)  │Write 3 │
└────────┘         └────────┘         └────────┘
 500μs              500μs              500μs

Throughput: ~32 MB/s per thread ❌
```

#### Solution: Parallel Writes (QD=32)

```
Timeline with parallel async writes:
┌────────┐
│Write 1 │
├────────┤
│Write 2 │  ← All submitted simultaneously
├────────┤
│Write 3 │
├────────┤
│  ...   │
├────────┤
│Write 32│
└────────┘

Throughput: 32 × 32 MB/s = 1 GB/s ✅
```

### Implementation Strategy

#### Option 1: Multiple Write Threads (Simple)

```cpp
// Increase write thread pool size
class raw_disk_io {
private:
    // Current: 8 threads
    boost::asio::thread_pool write_thread_pool_;

    // Proposed: 32-64 threads for NVMe
    // (auto-detected based on storage type)
};

// When flushing, dispatch to thread pool (already async)
void flush_pending_writes(storage_index_t storage) {
    // ... group contiguous writes ...

    for (auto& group : groups) {
        // This already posts to thread pool!
        boost::asio::post(write_thread_pool_, [=]() {
            dispatch_coalesced_write_slices(storage, group);
        });
        // Don't wait - post next immediately
    }
}
```

**Benefits:**
- ✅ Simple - minimal code change
- ✅ Works with existing pwritev
- ✅ NVMe can handle 32-64 concurrent requests
- ✅ No new dependencies

**Tuning:**
```cpp
size_t get_optimal_write_threads(storage_type type) {
    switch (type) {
    case storage_type::HDD:
        return 2;   // HDD serializes anyway (seek)

    case storage_type::SSD_SATA:
        return 8;   // Moderate parallelism

    case storage_type::SSD_NVME:
        return 32;  // High parallelism ✅
                    // NVMe supports 64K+ queue depth
    }
}

// In constructor:
raw_disk_io::raw_disk_io(...) {
    auto storage_type = detect_storage_type(device);
    size_t num_threads = get_optimal_write_threads(storage_type);
    write_thread_pool_.resize(num_threads);
}
```

#### Option 2: io_uring (Advanced, Phase 3)

```cpp
// io_uring: True async I/O (zero syscalls after setup)
#include <liburing.h>

class raw_disk_io {
private:
    io_uring ring_;  // NVMe can handle QD=64+
};

void flush_pending_writes(storage_index_t storage) {
    // ... group contiguous writes ...

    for (auto& group : groups) {
        // Submit to io_uring (doesn't block!)
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_writev(sqe, fd, iov, count, offset);
        io_uring_sqe_set_data(sqe, completion_callback);
    }

    // Submit all at once
    io_uring_submit(&ring_);

    // Completion handled by separate thread
}
```

**Benefits:**
- ✅ Zero-copy, zero syscalls after setup
- ✅ NVMe queue depth > 1 automatically
- ✅ Kernel-level batching and optimization

**Drawbacks:**
- ❌ Linux 5.1+ only
- ❌ More complex error handling
- ❌ Additional dependency (liburing)

**Recommendation:** Start with Option 1, evaluate Option 2 if needed.

---

### Expected Performance

#### Current (QD=1, 8 threads)

```
Per-thread throughput: 32 MB/s (syscall overhead)
8 threads:             256 MB/s
Total:                 ~500 MB/s ❌
Network utilization:   40%
```

#### With Parallel Writes (QD=32, 32 threads)

```
Per-thread throughput: 32 MB/s
32 threads:            1024 MB/s
Total:                 ~1 GB/s ✅
Network utilization:   80%
```

#### With io_uring (QD=64)

```
Queue depth:           64+ concurrent writes
NVMe saturation:       ~3 GB/s (hardware limit)
Bottleneck:            Network (1.25 GB/s) ✅
Network utilization:   100%
```

---

### Configuration Strategy

```cpp
// Auto-tune based on storage type and network speed
struct parallelism_config {
    size_t write_threads;
    size_t max_inflight_writes;
    bool use_io_uring;
};

parallelism_config get_parallelism_config(storage_type type) {
    switch (type) {
    case storage_type::HDD:
        return {
            .write_threads = 4,          // Low parallelism
            .max_inflight_writes = 8,
            .use_io_uring = false        // No benefit
        };

    case storage_type::SSD_SATA:
        return {
            .write_threads = 16,         // Moderate
            .max_inflight_writes = 32,
            .use_io_uring = false
        };

    case storage_type::SSD_NVME:
        return {
            .write_threads = 32,         // High parallelism ✅
            .max_inflight_writes = 64,   // Let NVMe queue up
            .use_io_uring = true         // Optional, if available
        };
    }
}
```

---

### Implementation Priority

**Phase 2.1: Write Coalescing** (Current Design)
- Benefit: Reduce syscall overhead
- Effort: 2-3 days
- Expected: HDD +93%, NVMe +20-30%

**Phase 2.2: Increase Write Thread Pool** (NEW - Easy Win!)
- Benefit: Saturate NVMe queue depth
- Effort: **1 day** (simple change!)
- Expected: NVMe +100-150% throughput ✅

**Phase 3: io_uring** (Optional)
- Benefit: Eliminate remaining syscall overhead
- Effort: 3-5 days (complex)
- Expected: NVMe +20-30% additional

**Recommendation:**
1. **Implement Phase 2.1 + 2.2 together** (3-4 days)
   - Coalescing reduces syscalls
   - More threads saturate NVMe
   - Combined effect: **2-3x NVMe throughput** ✅

2. Measure results with real deployments

3. Consider io_uring only if still not saturating network

---

## Appendix: Example Configuration for Clonezilla

```python
# In Clonezilla deployment script
import ezio_pb2

# Detect storage type
storage_type = detect_storage_type(target_device)

request = ezio_pb2.AddRequest()

if storage_type == "HDD":
    # Aggressive coalescing for HDD
    settings.set_bool(settings_pack.enable_write_coalescing, True)
    settings.set_int(settings_pack.write_coalesce_max_blocks, 64)
    settings.set_int(settings_pack.write_coalesce_timeout_ms, 150)
    settings.set_int(settings_pack.write_coalesce_min_count, 4)
elif storage_type == "SATA_SSD":
    # Moderate coalescing
    settings.set_int(settings_pack.write_coalesce_timeout_ms, 50)
    settings.set_int(settings_pack.write_coalesce_max_blocks, 32)
elif storage_type == "NVME":
    # Minimal coalescing
    settings.set_int(settings_pack.write_coalesce_timeout_ms, 5)
    settings.set_int(settings_pack.write_coalesce_max_blocks, 16)
    settings.set_int(settings_pack.write_coalesce_min_count, 8)
else:
    # Unknown - disable coalescing
    settings.set_bool(settings_pack.enable_write_coalescing, False)

stub.AddTorrent(request)
```

---

**Document Version:** 1.0
**Last Updated:** 2024-12-14
**Status:** Ready for Implementation Review
