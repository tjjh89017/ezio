# Store Buffer Watermark & Backpressure Design

**Version:** 1.0
**Date:** 2024-12-14
**Status:** Design Phase

---

## Problem Statement

**Current Issue:**
- `store_buffer` has NO size limit (unbounded `unordered_map`)
- If writes are slower than network receives, `store_buffer` grows indefinitely
- 128MB buffer pool may be insufficient for high-throughput scenarios

**Questions:**
1. How large should buffer pool be?
2. When should we trigger urgent flush?
3. When should we force synchronous write (block libtorrent)?

---

## libtorrent 2.x Design Analysis

### Buffer Pool Sizing

**Important Clarification:**
- libtorrent 2.0+ removed internal disk cache
- `cache_size` setting is **deprecated** in libtorrent 2.x
- `disk_buffer_pool` is **NOT a cache**, it's a buffer allocator
- `store_buffer` is **NOT a cache**, it's pending write tracking

**libtorrent 2.x** (`disk_buffer_pool.cpp`):
```cpp
// Watermark calculation (line 166-170)
if (m_in_use >= m_low_watermark + (m_max_use - m_low_watermark) / 2
    && !m_exceeded_max_size) {
    m_exceeded_max_size = true;  // Trigger at 75%
}

// Watermark values:
m_low_watermark = m_max_use / 2;     // 50%
high_watermark = low + (max-low)/2;  // 75%
```

**Buffer pool is for I/O operations, NOT caching:**
- Used to allocate buffers for async_read/async_write
- Buffers are immediately freed after I/O completes
- No data caching - relies on OS page cache

**EZIO current:**
- MAX_BUFFER_POOL_SIZE = 128 MB (8192 blocks)
- LOW_WATERMARK = 4096 blocks (50%)
- HIGH_WATERMARK = 7168 blocks (87.5%) ← Higher than libtorrent's 75%!

**Why EZIO needs larger buffer pool:**
- 10 Gbps network (much faster than typical desktop use)
- Need buffer for burst traffic
- store_buffer holds data until write completes (adds pressure)

---

## Recommended Configuration

### Buffer Pool Sizing

**Considerations:**
1. **Network throughput**: 10 Gbps = 1.25 GB/s
2. **Write latency**: HDD ~13ms, SSD ~500μs, NVMe ~100μs
3. **Pending writes capacity**: How many blocks can be in-flight?

**Calculation:**
```
10 Gbps network, 16KB blocks:
  → 78,125 blocks/sec throughput

HDD write (with coalescing):
  → 60ms per 64-block batch = ~1000 batches/sec
  → 64,000 blocks/sec capacity ✅ Can keep up!

But during burst:
  → Network can send 64 blocks in ~8ms
  → HDD takes 60ms to write
  → Backlog: 64 blocks every 52ms = ~1230 blocks/sec accumulation

Safe buffer size = burst capacity + margin:
  → 10 seconds of burst = 12,300 blocks = 192 MB
  → Recommendation: 256 MB (16,384 blocks)
```

**Proposed Configuration:**
```cpp
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)  // 256 MB (up from 128 MB)
#define DEFAULT_BLOCK_SIZE (16 * 1024)               // 16 KB

#define BUFFER_COUNT (MAX_BUFFER_POOL_SIZE / DEFAULT_BLOCK_SIZE)  // 16384
#define LOW_WATERMARK (BUFFER_COUNT / 2)                          // 8192 (50%)
#define HIGH_WATERMARK (LOW_WATERMARK + (BUFFER_COUNT - LOW_WATERMARK) * 3 / 4)
                                                                   // 14336 (87.5%)
```

### Adaptive Sizing (Optional)

**Auto-detect network speed:**
```cpp
size_t calculate_buffer_pool_size() {
    // Detect network interface speed
    auto network_speed = detect_network_speed();  // e.g., 10Gbps

    // Calculate blocks per second
    size_t blocks_per_sec = (network_speed / 8) / DEFAULT_BLOCK_SIZE;

    // Buffer for 10 seconds of burst
    size_t burst_blocks = blocks_per_sec * 10;

    // Round up to power of 2
    size_t recommended = next_power_of_2(burst_blocks);

    // Clamp between 4096 and 65536 blocks (64MB - 1GB)
    return std::clamp(recommended, 4096UL, 65536UL);
}
```

---

## Three-Level Backpressure Design

### Level 1: Normal Flush (Coalescing)

**Trigger Conditions:**
```cpp
void check_flush_conditions(storage_index_t storage) {
    auto& pending = m_pending_writes[storage];
    auto const& config = m_coalesce_configs[storage];

    bool should_flush = false;

    // Condition 1: Accumulated enough blocks
    if (pending.size() >= config.max_pending_blocks) {
        should_flush = true;
    }

    // Condition 2: Timeout expired (HDD only)
    if (config.timeout > 0ms && !pending.empty()) {
        auto age = now() - pending.front().enqueue_time;
        if (age > config.timeout) {
            should_flush = true;
        }
    }

    if (should_flush) {
        flush_pending_writes(storage);  // Normal async flush
    }
}
```

**Behavior:** Async flush to thread pool, optimal for performance.

---

### Level 2: Urgent Flush (Pressure)

**Trigger Conditions:**
```cpp
void async_write(storage_index_t storage, peer_request const& r,
                 char const* buf, std::function<void(storage_error const&)> handler) {
    // Allocate buffer
    bool exceeded = false;
    char* buffer = m_buffer_pool.allocate_buffer(exceeded);

    if (!buffer) {
        // Critical: Out of memory!
        handler(storage_error{errors::no_memory, operation_t::alloc_cache_piece});
        return;
    }

    // Copy data
    memcpy(buffer, buf, r.length);

    // Insert to store_buffer
    torrent_location loc{storage, r.piece, r.start};
    m_store_buffer.insert(loc, buffer);

    // Enqueue for coalescing
    enqueue_write(storage, {loc, buffer, handler, now()});

    // Check pressure levels
    size_t pending_count = m_pending_writes[storage].size();
    size_t buffer_usage = m_buffer_pool.size();

    // Level 2: Urgent flush conditions
    bool urgent = false;

    // Condition A: Buffer pool exceeded watermark
    if (exceeded) {
        urgent = true;
    }

    // Condition B: Too many pending writes
    if (pending_count > MAX_PENDING_WRITES_PER_STORAGE) {
        urgent = true;
    }

    // Condition C: Store buffer size concern
    size_t total_pending = 0;
    for (auto const& [s, writes] : m_pending_writes) {
        total_pending += writes.size();
    }
    if (total_pending > BUFFER_COUNT * 0.8) {  // 80% of pool
        urgent = true;
    }

    if (urgent) {
        // Urgent: Flush immediately, don't wait for timeout
        flush_pending_writes(storage);  // Still async, but immediate
    } else {
        // Normal: Wait for accumulation or timeout
        check_flush_conditions(storage);
    }
}
```

**Thresholds:**
```cpp
// Per-storage pending write limit
#define MAX_PENDING_WRITES_PER_STORAGE 512  // 8 MB worth

// Total pending limit (across all storages)
// Should be less than buffer pool size
#define MAX_TOTAL_PENDING_RATIO 0.8  // 80% of buffer pool
```

**Behavior:** Still async flush, but triggered immediately without waiting.

---

### Level 3: Synchronous Write (Critical)

**Trigger Condition:**
```cpp
void async_write(storage_index_t storage, peer_request const& r,
                 char const* buf, std::function<void(storage_error const&)> handler) {
    // ... (allocate, copy, insert to store_buffer) ...

    // Check CRITICAL conditions
    size_t pending_count = m_pending_writes[storage].size();

    // Level 3: Force synchronous write
    bool force_sync = false;

    // Condition: Pending writes exceed HARD limit
    if (pending_count > MAX_PENDING_WRITES_PER_STORAGE * 2) {
        force_sync = true;
        SPDLOG_WARN("Force sync write: pending={}, storage={}",
                    pending_count, static_cast<int>(storage));
    }

    if (force_sync) {
        // CRITICAL: Write synchronously, block libtorrent
        // DO NOT enqueue, write immediately

        storage_error error;
        auto* ps = m_storages[storage].get();
        ps->write(const_cast<char*>(buffer), r.piece, r.start, r.length, error);

        // Remove from store_buffer immediately
        m_store_buffer.erase(loc);
        m_buffer_pool.free_disk_buffer(buffer);

        // Call handler on current thread (blocks libtorrent)
        handler(error);

        return;  // Don't enqueue!
    }

    // Normal/Urgent path: Enqueue for async write
    enqueue_write(storage, {loc, buffer, handler, now()});
    // ... check flush conditions ...
}
```

**Hard Limits:**
```cpp
#define MAX_PENDING_WRITES_PER_STORAGE 512        // Soft limit → urgent flush
#define CRITICAL_PENDING_WRITES_PER_STORAGE 1024  // Hard limit → sync write
```

**Behavior:**
- Blocks libtorrent's async_write call
- Writes directly to disk (synchronous pwrite)
- Prevents unbounded memory growth
- Only happens in extreme cases (disk much slower than network)

---

## Complete Flow Diagram

```
async_write() called
    ↓
Allocate buffer from buffer_pool
    ↓
    ├─ exceeded=false ─────────┐
    │                          ↓
    └─ exceeded=true ──→ [URGENT FLAG]
         (75% full)
    ↓
Copy data to buffer
    ↓
Insert to store_buffer
    ↓
Check pending write count
    ↓
    ├─ < 512 writes ──────────────────┐ [NORMAL]
    │                                  ↓
    ├─ 512-1024 writes ───────────────┤ [URGENT]
    │    OR exceeded=true              │
    │                                  ↓
    └─ > 1024 writes ──→ [CRITICAL - SYNC WRITE]
                              ↓
                        Direct pwrite()
                        Block libtorrent
                        Return immediately
                              ↓
                          [DONE]

[NORMAL/URGENT path]:
    ↓
Enqueue to m_pending_writes
    ↓
    ├─ [URGENT] ──→ flush_pending_writes()  (immediate)
    │                    ↓
    │               dispatch to thread pool
    │
    └─ [NORMAL] ──→ check_flush_conditions()
                         ↓
                    ├─ timeout expired ──→ flush
                    ├─ accumulated enough ─→ flush
                    └─ otherwise ──→ wait
                         ↓
                    schedule timer
                         ↓
                    [async write in background]
```

---

## Implementation Details

### Track Pending Count Efficiently

```cpp
class raw_disk_io {
private:
    // Pending writes per storage
    std::map<storage_index_t, std::vector<pending_write>> m_pending_writes;
    std::mutex m_pending_mutex;

    // Track total pending (cached, no need to iterate)
    std::atomic<size_t> m_total_pending_count{0};

    // Configuration
    struct backpressure_config {
        size_t urgent_threshold = 512;      // Per-storage soft limit
        size_t critical_threshold = 1024;   // Per-storage hard limit
        float total_ratio = 0.8f;           // 80% of buffer pool
    } m_backpressure_config;
};

void enqueue_write(storage_index_t storage, pending_write&& pw) {
    std::unique_lock<std::mutex> l(m_pending_mutex);
    m_pending_writes[storage].push_back(std::move(pw));
    m_total_pending_count++;
    // Don't hold lock while checking conditions
    size_t pending_count = m_pending_writes[storage].size();
    l.unlock();

    // Check outside of lock
    check_backpressure(storage, pending_count);
}

void flush_completed(storage_index_t storage, size_t count) {
    m_total_pending_count -= count;
}
```

### Metrics & Monitoring

```cpp
struct disk_io_stats {
    // Counters
    std::atomic<uint64_t> normal_flushes{0};
    std::atomic<uint64_t> urgent_flushes{0};
    std::atomic<uint64_t> sync_writes{0};    // Should be RARE!

    // Gauges
    std::atomic<size_t> current_pending{0};
    std::atomic<size_t> max_pending_seen{0};
    std::atomic<size_t> buffer_pool_usage{0};

    // Log warning if sync_writes > 0
    void check_health() {
        if (sync_writes > 0) {
            SPDLOG_WARN("Sync writes occurred: {} times - disk may be bottleneck!",
                       sync_writes.load());
        }
    }
};
```

---

## Configuration Recommendations

### By Deployment Scenario

**Small deployment (< 10 clients):**
```cpp
MAX_BUFFER_POOL_SIZE = 128 MB  // 8192 blocks
urgent_threshold = 256
critical_threshold = 512
```

**Medium deployment (10-50 clients):**
```cpp
MAX_BUFFER_POOL_SIZE = 256 MB  // 16384 blocks (recommended)
urgent_threshold = 512
critical_threshold = 1024
```

**Large deployment (50+ clients):**
```cpp
MAX_BUFFER_POOL_SIZE = 512 MB  // 32768 blocks
urgent_threshold = 1024
critical_threshold = 2048
```

**Auto-detect (recommended):**
```cpp
// Detect network speed and set buffer accordingly
auto buffer_size = calculate_buffer_pool_size();
urgent_threshold = buffer_size / 32;    // ~3% of pool
critical_threshold = buffer_size / 16;  // ~6% of pool
```

---

## Testing Strategy

### Unit Tests

**Test 1: Normal flow (no pressure)**
```cpp
TEST(backpressure, normal_flow) {
    // Write 100 blocks
    for (int i = 0; i < 100; ++i) {
        async_write(storage, piece, offset, buf, handler);
    }

    // Should NOT trigger sync writes
    EXPECT_EQ(stats.sync_writes, 0);

    // Should accumulate in pending
    EXPECT_GT(pending_count(), 0);
}
```

**Test 2: Urgent flush**
```cpp
TEST(backpressure, urgent_flush) {
    // Fill buffer pool to 80%
    fill_buffer_pool(0.8);

    // Next write should trigger urgent flush
    async_write(storage, piece, offset, buf, handler);

    // Should flush immediately
    wait_for_flush();
    EXPECT_EQ(stats.urgent_flushes, 1);
    EXPECT_EQ(stats.sync_writes, 0);  // Still async!
}
```

**Test 3: Critical sync write**
```cpp
TEST(backpressure, critical_sync) {
    // Mock slow disk (delay all flushes)
    mock_slow_disk(10000ms);

    // Write beyond critical threshold
    for (int i = 0; i < 1500; ++i) {
        async_write(storage, piece, offset, buf, handler);
    }

    // Should trigger sync writes
    EXPECT_GT(stats.sync_writes, 0);
    SPDLOG_WARN("Sync writes: {}", stats.sync_writes.load());
}
```

---

## FAQ

### Q1: Why 256 MB instead of 128 MB?

**A:** 10 Gbps network can burst faster than HDD writes. 256 MB provides buffer for ~10 seconds of burst traffic, preventing sync writes.

### Q2: Will sync writes hurt performance?

**A:** Yes, but they prevent memory exhaustion. If sync writes occur frequently, it means:
1. Disk is too slow for network speed
2. Need faster storage (NVMe)
3. Need to limit network bandwidth
4. Increase write thread pool (Phase 2.2)

### Q3: What if buffer pool is too large (memory limited)?

**A:** Lower thresholds:
```cpp
MAX_BUFFER_POOL_SIZE = 64 MB   // Minimum
urgent_threshold = 128
critical_threshold = 256
```

But: More likely to hit sync writes under load.

### Q4: Difference from libtorrent 2.x?

**A:** libtorrent 2.x relies on application to handle backpressure (pause torrent). EZIO implements it internally with 3-level system.

---

## References

- `libtorrent-2.0.10/src/disk_buffer_pool.cpp` (watermark implementation)
- `docs/MUTEX_ANALYSIS.md` (buffer pool analysis)
- `docs/WRITE_COALESCING_DESIGN.md` (write optimization)

---

**Document Version:** 1.0
**Last Updated:** 2024-12-14
**Status:** Ready for Review
