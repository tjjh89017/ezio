# Future Optimizations (Low Priority)

**Version:** 1.0
**Date:** 2024-12-14
**Status:** Ideas for Future Work
**Priority:** ⬇️ Low (after Phase 1 & 2 complete)

---

## Introduction

This document lists potential optimizations discovered from libtorrent 2.x documentation and source code analysis. These are **lower priority** items to consider after completing the main optimization phases.

**Reference:** libtorrent 2.0.10 `/docs/tuning.rst`, `/docs/features.rst`

---

## 1. Parallel Hashing (Medium Effort, High Benefit for Multi-Core)

### Current State
- EZIO uses single-threaded hashing in `hash_thread_pool_`
- Hash computation can be CPU bottleneck at high speeds

### libtorrent 2.x Support
```cpp
// settings_pack (settings_pack.hpp:1736-1746)
hashing_threads  // Number of threads for piece hash computation
aio_threads      // Number of disk I/O threads
```

### Proposal
```cpp
// raw_disk_io.hpp
class raw_disk_io {
private:
    // Current: Single pool for hashing
    boost::asio::thread_pool hash_thread_pool_{8};

    // Proposed: Separate pools, configurable
    boost::asio::thread_pool m_aio_thread_pool{8};      // Disk I/O
    boost::asio::thread_pool m_hash_thread_pool{4};     // SHA-1 computation
};
```

**Benefits:**
- HDD: Minimal (disk is bottleneck)
- NVMe: +30-50% throughput at high speed (> 5 Gbps)
- Multi-core utilization improves

**Implementation Effort:** 2-3 days

**When to Consider:**
- After Phase 2.2 (parallel writes) complete
- When CPU becomes bottleneck (profile first!)
- When deploying on multi-core servers (8+ cores)

---

## 2. Adaptive Thread Pool Sizing (Low Effort, Moderate Benefit)

### Current State
- Fixed thread counts: read=8, write=8, hash=8
- No adaptation to workload or hardware

### libtorrent 2.x Approach
```cpp
// Auto-configured based on:
// 1. Number of CPU cores
// 2. Number of active torrents
// 3. Storage type (HDD vs SSD)
```

### Proposal
```cpp
struct thread_pool_config {
    storage_type type;
    size_t cpu_cores;

    auto get_optimal_threads() const {
        switch (type) {
        case storage_type::HDD:
            return thread_counts{
                .read = std::min(cpu_cores / 2, 8UL),
                .write = 2,  // HDD serializes anyway
                .hash = std::min(cpu_cores / 4, 4UL)
            };

        case storage_type::SSD_NVME:
            return thread_counts{
                .read = std::min(cpu_cores, 16UL),
                .write = std::min(cpu_cores * 2, 32UL),  // High parallelism
                .hash = std::min(cpu_cores / 2, 8UL)
            };
        }
    }
};
```

**Benefits:**
- Better resource utilization
- Adapts to different hardware
- Easier deployment (less manual tuning)

**Implementation Effort:** 1 day

**When to Consider:**
- After basic optimizations stabilize
- When supporting diverse hardware configurations

---

## 3. Memory Pool for Disk Buffers (High Effort, Moderate Benefit)

### Current State
```cpp
char* buffer_pool::allocate_buffer_impl() {
    char* buf = static_cast<char*>(malloc(DEFAULT_BLOCK_SIZE));
    ++m_size;
    return buf;
}

void buffer_pool::free_disk_buffer(char* buf) {
    free(buf);
    --m_size;
}
```

**Issue:** Each allocation/deallocation goes to system allocator
- malloc/free overhead: ~100-500 ns per call
- Memory fragmentation over time
- No cache-line alignment guarantees

### libtorrent 2.x Approach
libtorrent 2.x still uses malloc/free but could benefit from pooling.

### Proposal: Custom Memory Pool
```cpp
class aligned_buffer_pool {
private:
    // Pre-allocated buffer arena
    std::vector<char*> m_free_list;
    std::vector<std::unique_ptr<char[]>> m_arenas;

    static constexpr size_t BUFFER_SIZE = 16384;
    static constexpr size_t ALIGNMENT = 4096;  // Page-aligned
    static constexpr size_t ARENA_SIZE = 64 * 1024 * 1024;  // 64 MB chunks

public:
    char* allocate_buffer() {
        std::unique_lock<std::mutex> l(m_mutex);

        if (m_free_list.empty()) {
            // Allocate new arena
            auto arena = std::make_unique<char[]>(ARENA_SIZE);

            // Split arena into buffers
            for (size_t i = 0; i < ARENA_SIZE / BUFFER_SIZE; ++i) {
                m_free_list.push_back(arena.get() + i * BUFFER_SIZE);
            }

            m_arenas.push_back(std::move(arena));
        }

        char* buf = m_free_list.back();
        m_free_list.pop_back();
        return buf;
    }

    void free_buffer(char* buf) {
        std::unique_lock<std::mutex> l(m_mutex);
        m_free_list.push_back(buf);
    }
};
```

**Benefits:**
- Faster allocation: ~10-50 ns (vs 100-500 ns)
- No fragmentation
- Cache-line aligned buffers (better performance)
- Predictable memory usage

**Drawbacks:**
- More complex code
- Memory not returned to OS (stays in pool)
- Need to tune arena size

**Implementation Effort:** 3-4 days

**When to Consider:**
- After profiling shows malloc/free overhead > 5%
- For extremely high throughput (> 20 Gbps)
- When running 24/7 servers (fragmentation concern)

---

## 4. Zero-Copy Networking (High Effort, High Benefit)

### Current State
```cpp
async_write() {
    // 1. Allocate buffer from pool
    char* buffer = allocate_buffer();

    // 2. Copy data from network buffer
    memcpy(buffer, network_buf, length);  // ← COPY!

    // 3. Store in store_buffer
    m_store_buffer.insert(loc, buffer);

    // 4. Eventually write to disk
}
```

**Issue:** Data is copied from network buffer to disk buffer.

### Proposal: Direct Buffer Passing
```cpp
// Use libtorrent's buffer directly
async_write(storage_index_t storage, peer_request const& r,
            disk_buffer_holder buffer,  // ← Already owns memory
            std::function<void(storage_error const&)> handler) {

    // No copy! Just transfer ownership
    char* buf = buffer.release();

    m_store_buffer.insert(loc, buf);
    enqueue_write(storage, {loc, buf, handler, now()});
}
```

**Benefits:**
- Eliminate one memcpy (16 KB copy ~5-10 μs)
- Reduce memory pressure
- ~10-15% CPU reduction

**Challenges:**
- Requires libtorrent API change (disk_interface signature)
- Need to ensure buffer ownership is clear
- May need custom allocator that libtorrent understands

**Implementation Effort:** 5-7 days

**When to Consider:**
- After other optimizations complete
- When CPU usage is still high (> 50%)
- For 10+ Gbps deployments

---

## 5. Direct I/O (O_DIRECT) (Medium Effort, Complex Trade-offs)

### Current State
- Uses buffered I/O (relies on OS page cache)
- No O_DIRECT flag

### Proposal
```cpp
class partition_storage {
    int fd_;
    bool m_use_direct_io = false;

public:
    partition_storage(const std::string& path, bool direct_io) {
        int flags = O_RDWR;
        if (direct_io) {
            flags |= O_DIRECT;  // Bypass page cache
        }
        fd_ = open(path.c_str(), flags);
    }
};
```

**Benefits:**
- Avoids double-buffering (page cache + our buffer)
- More predictable I/O latency
- Better control over memory usage

**Drawbacks:**
- ⚠️ Must align all I/O to 512B/4KB boundaries
- ⚠️ Must use aligned buffers (posix_memalign)
- ⚠️ No read-ahead from OS
- ⚠️ Worse performance for random reads
- ⚠️ Complex implementation

**Verdict:** ❌ **Not Recommended for EZIO**

**Reasons:**
1. EZIO's sequential write pattern benefits from OS page cache
2. Alignment requirements complicate write coalescing
3. OS does good job of write-behind caching
4. No clear benefit over current approach

---

## 6. io_uring Support (High Effort, Cutting Edge)

### What is io_uring?
- Modern Linux async I/O interface (kernel 5.1+)
- Zero-copy, zero-syscall I/O submission
- Much faster than traditional read/write/pread/pwrite

### Proposal
```cpp
#ifdef HAS_IO_URING
class io_uring_storage : public partition_storage {
private:
    io_uring m_ring;

public:
    void writev(std::vector<write_request> const& requests) override {
        // Prepare SQEs (submission queue entries)
        for (auto const& req : requests) {
            io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
            io_uring_prep_writev(sqe, fd_, iov, iovcnt, offset);
        }

        // Submit all at once (single syscall!)
        io_uring_submit(&m_ring);

        // Wait for completions
        io_uring_wait_cqe(&m_ring, &cqe);
    }
};
#endif
```

**Benefits:**
- NVMe: +50-100% IOPS (lower latency)
- Reduced CPU overhead (fewer syscalls)
- Better parallelism

**Challenges:**
- Requires Linux 5.1+ (not portable)
- Complex error handling
- Need fallback for older kernels
- Limited adoption/testing

**Implementation Effort:** 7-10 days

**When to Consider:**
- After Phase 2 complete and deployed
- When targeting cutting-edge hardware (Gen4+ NVMe)
- When deploying on recent Linux (Ubuntu 22.04+)

---

## 7. Monitoring & Telemetry (Low Effort, High Value)

### Current State
- Minimal metrics
- Only spdlog for logging

### Proposal: Comprehensive Metrics
```cpp
struct disk_io_metrics {
    // Throughput
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> bytes_written{0};

    // Latency histograms
    histogram read_latency;
    histogram write_latency;

    // Queue depths
    std::atomic<size_t> pending_reads{0};
    std::atomic<size_t> pending_writes{0};

    // Buffer pool stats
    std::atomic<size_t> buffer_allocations{0};
    std::atomic<size_t> buffer_pool_pressure{0};

    // Coalescing effectiveness
    std::atomic<uint64_t> coalesced_writes{0};
    std::atomic<uint64_t> single_writes{0};
    std::atomic<uint64_t> sync_writes{0};  // Should be ~0!

    // Export to Prometheus/StatsD
    void export_metrics();
};
```

**Benefits:**
- Identify bottlenecks in production
- Track optimization effectiveness
- Early warning for issues
- Data-driven tuning

**Implementation Effort:** 2-3 days

**When to Consider:**
- Implement alongside Phase 2
- Critical for production deployment
- Helps validate optimization claims

**Tools to Consider:**
- Prometheus (metrics)
- Grafana (visualization)
- perf (profiling)

---

## 8. Settings Configuration (libtorrent Integration)

### Current State
- No integration with libtorrent settings
- EZIO uses hardcoded values

### libtorrent Settings Relevant to EZIO

From `settings_pack.hpp`:

```cpp
// Disk I/O threads
aio_threads              // Number of disk I/O threads
hashing_threads          // Number of hashing threads

// Memory
max_queued_disk_bytes    // Max bytes queued for disk I/O
cache_size               // DEPRECATED in 2.x

// Performance
send_buffer_watermark    // When to read from disk for sending
recv_socket_buffer_size  // Kernel recv buffer size
send_socket_buffer_size  // Kernel send buffer size

// Timeouts
request_timeout          // Block request timeout
peer_timeout             // Peer connection timeout
inactivity_timeout       // Connection inactivity timeout

// Connections
connections_limit        // Max total connections
unchoke_slots_limit      // Max upload slots
```

### Proposal: Expose EZIO Settings via settings_pack

```cpp
// settings_pack.hpp (custom extension)
namespace ezio_settings {
    // Buffer pool
    int_type_t buffer_pool_size;          // 256 MB default
    int_type_t buffer_low_watermark;      // 50%
    int_type_t buffer_high_watermark;     // 87.5%

    // Write coalescing
    bool_type_t write_coalescing_enabled;
    int_type_t max_pending_blocks;        // 64 blocks
    int_type_t min_coalesce_count;        // 4 blocks
    int_type_t write_coalesce_timeout;    // 150ms for HDD, 0ms for NVMe

    // Backpressure
    int_type_t urgent_flush_threshold;    // 512 blocks
    int_type_t critical_sync_threshold;   // 1024 blocks

    // Thread pools
    int_type_t read_threads;              // 8
    int_type_t write_threads;             // 8 (or 32 for NVMe)
    int_type_t hash_threads;              // 8
}

// raw_disk_io.cpp
void raw_disk_io::settings_updated() {
    auto const& s = m_settings;

    // Update buffer pool
    m_buffer_pool.set_max_size(
        s.get_int(ezio_settings::buffer_pool_size)
    );

    // Update write coalescing config
    m_coalesce_config.enabled =
        s.get_bool(ezio_settings::write_coalescing_enabled);
    m_coalesce_config.max_pending_blocks =
        s.get_int(ezio_settings::max_pending_blocks);

    // ... etc
}
```

**Benefits:**
- Runtime configuration without restart
- Consistent with libtorrent API
- Easy A/B testing
- Can tune per-deployment

**Implementation Effort:** 2-3 days

**When to Consider:**
- After Phase 1.2 (configurable cache) implemented
- When supporting multiple deployment scenarios

---

## 9. Storage Type Auto-Detection

### Current State
- No automatic detection of storage type (HDD vs SSD vs NVMe)
- Manual configuration required

### Proposal
```cpp
enum class storage_type {
    HDD,
    SSD_SATA,
    SSD_NVME,
    UNKNOWN
};

storage_type detect_storage_type(const std::string& device_path) {
    // Method 1: Check rotational flag
    std::string rotational_path =
        "/sys/block/" + device + "/queue/rotational";
    int is_rotational = read_sysfs_int(rotational_path);

    if (is_rotational == 1) {
        return storage_type::HDD;
    }

    // Method 2: Check if NVMe
    if (device_path.find("/dev/nvme") != std::string::npos) {
        return storage_type::SSD_NVME;
    }

    // Method 3: Benchmark (measure 4KB random read latency)
    auto latency = benchmark_random_read(device_path);
    if (latency < 100us) {
        return storage_type::SSD_NVME;
    } else if (latency < 1ms) {
        return storage_type::SSD_SATA;
    } else {
        return storage_type::HDD;
    }
}

// Auto-configure based on type
void auto_configure(storage_type type) {
    switch (type) {
    case storage_type::HDD:
        m_write_threads = 2;
        m_coalesce_timeout = 150ms;
        break;

    case storage_type::SSD_NVME:
        m_write_threads = 32;
        m_coalesce_timeout = 0ms;
        break;
    }
}
```

**Benefits:**
- Zero-configuration deployment
- Optimal settings out-of-box
- Less user error

**Implementation Effort:** 2-3 days

**When to Consider:**
- After manual tuning parameters stabilized
- When deploying to diverse environments

---

## Priority Matrix

| Optimization | Effort | Benefit | Priority | When |
|-------------|--------|---------|----------|------|
| 1. Parallel Hashing | Medium | High | Medium | After Phase 2.2 |
| 2. Adaptive Thread Pools | Low | Medium | Low | After stabilization |
| 3. Memory Pool | High | Medium | Low | If malloc overhead > 5% |
| 4. Zero-Copy Network | High | High | Medium | After Phase 2 |
| 5. Direct I/O (O_DIRECT) | Medium | Negative | ❌ Never | Not recommended |
| 6. io_uring | Very High | Very High | Low | Cutting edge only |
| 7. Monitoring/Metrics | Low | Very High | **High** | Alongside Phase 2 |
| 8. Settings Integration | Low | Medium | Medium | After Phase 1.2 |
| 9. Storage Auto-Detect | Medium | Medium | Medium | After tuning stable |

**Recommended Order:**
1. ✅ **Phase 1 & 2** (current focus)
2. 🔥 **Monitoring & Metrics** (implement during Phase 2)
3. **Settings Integration** (after Phase 1.2)
4. **Parallel Hashing** (if CPU bottleneck)
5. **Zero-Copy Network** (if CPU usage high)
6. **Storage Auto-Detect** (quality of life)
7. Others as needed based on profiling

---

---

## 10. Advanced Disk I/O Architecture (Insights from libtorrent)

### libtorrent 2.x Disk I/O Design

From `manual.rst` and `features.rst`, libtorrent 2.x uses a sophisticated disk I/O approach:

#### Memory-Mapped I/O with Store Buffer
```
Network → Receive Buffer (page-aligned) → Decrypt in-place → Store Buffer → writev() → Disk
         └─ No copy! ─────────────────────┘                              └─ Single syscall
```

**Key Insights:**
1. **Page-aligned buffers** - Received directly into disk buffers (no copy)
2. **In-place decryption** - Encryption/decryption done in buffer (EZIO: no encryption)
3. **Store buffer** - Holds blocks until piece complete or cache flush needed
4. **Single writev()** - All blocks for a piece written in one syscall

#### Read Path Optimization
```
Disk → mmap/page cache → Send buffer (copy for alignment) → Encrypt → iovec chain → send()
                        └─ One copy (for unaligned requests) ─────────┘
```

### Current EZIO vs libtorrent Comparison

| Aspect | libtorrent 2.x | EZIO Current | Gap |
|--------|----------------|--------------|-----|
| Receive buffer | Page-aligned, direct | Unknown alignment | Need verification |
| Store buffer | Yes (per-piece) | Yes (store_buffer) | ✅ Similar |
| Write coalescing | writev() multiple blocks | Planned Phase 2 | 🔄 In progress |
| Read cache | OS page cache | OS page cache | ✅ Same |
| Memory mapping | Yes (files) | No (raw device) | ⚠️ N/A for raw device |

### Recommendations for EZIO

#### 10.1 Page-Aligned Buffer Allocation
```cpp
// Ensure all buffers are page-aligned for optimal I/O
char* buffer_pool::allocate_buffer_impl() {
    void* buf = nullptr;

    // Allocate page-aligned buffer (4KB alignment)
    int ret = posix_memalign(&buf, 4096, DEFAULT_BLOCK_SIZE);
    if (ret != 0) {
        throw std::bad_alloc();
    }

    ++m_size;
    return static_cast<char*>(buf);
}
```

**Benefits:**
- Better performance for O_DIRECT if ever needed
- More efficient DMA transfers
- Better cache line alignment

**Implementation Effort:** 1 day

#### 10.2 Store Buffer with Piece-Level Tracking
```cpp
// From libtorrent insight: track completion per piece
class store_buffer_tracker {
private:
    struct piece_entry {
        std::vector<char*> blocks;
        size_t expected_blocks;
        size_t received_blocks;
        bool is_complete() const { return received_blocks == expected_blocks; }
    };

    std::unordered_map<piece_index_t, piece_entry> m_pieces;

public:
    void insert(piece_index_t piece, block_index_t block, char* buffer) {
        auto& entry = m_pieces[piece];
        entry.blocks[block] = buffer;
        entry.received_blocks++;

        if (entry.is_complete()) {
            // Trigger immediate flush of complete piece
            flush_piece(piece);
        }
    }

    void flush_piece(piece_index_t piece) {
        // Use writev() to write all blocks at once
        auto& entry = m_pieces[piece];
        std::vector<iovec> iov;
        iov.reserve(entry.blocks.size());

        for (auto* buf : entry.blocks) {
            iov.push_back({buf, DEFAULT_BLOCK_SIZE});
        }

        // Single syscall!
        pwritev(fd_, iov.data(), iov.size(), piece_offset);
    }
};
```

**Benefits:**
- Matches libtorrent's proven design
- Natural integration with write coalescing
- Piece-level completion tracking helps prioritization

**Implementation Effort:** 3-4 days
**When:** During Phase 2.1 (write coalescing)

---

## 11. Socket Buffer Tuning (From tuning.rst)

### Current State
- Unknown socket buffer sizes
- Likely using OS defaults (typically 128 KB - 256 KB)

### libtorrent Settings
```cpp
// settings_pack.hpp
recv_socket_buffer_size  // Kernel recv buffer per socket
send_socket_buffer_size  // Kernel send buffer per socket
```

### Recommendation for EZIO

For 10 Gbps network with 50 peers:

```cpp
// Calculate optimal socket buffers
// BDP = Bandwidth * RTT
// For 10Gbps / 50 peers = 200 Mbps per peer
// RTT = 50ms (typical WAN)
// BDP = 200 Mbps * 50ms = 10 MB / 8 = 1.25 MB

const int SOCKET_BUFFER_SIZE = 2 * 1024 * 1024;  // 2 MB (rounded up)

// Apply to all peer sockets
setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_SIZE, sizeof(int));
setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &SOCKET_BUFFER_SIZE, sizeof(int));
```

**Impact:**
- Prevents network stalls on high-latency connections
- Better utilization of 10 Gbps network
- Reduces packet loss from buffer overflow

**Tuning Formula:**
```
Optimal Buffer = BDP * 2
Where BDP = (Link Speed / Peers) * RTT

Examples:
- LAN (1ms RTT):   200 Mbps * 1ms  = 25 KB  → Use 64 KB
- WAN (50ms RTT):  200 Mbps * 50ms = 1.25 MB → Use 2 MB
- Intercontinental (200ms): 200 Mbps * 200ms = 5 MB → Use 8 MB
```

**Implementation Effort:** 1 day
**Priority:** Medium (do after Phase 1)

---

## 12. Request Queue Management (From streaming.rst)

### Insight from libtorrent Streaming

libtorrent has sophisticated logic for time-critical piece requests (streaming). While EZIO isn't streaming, the **request queue management** principles apply:

#### Download Queue Time Tracking
```cpp
// Track per-peer outstanding requests
struct peer_metrics {
    size_t outstanding_bytes;     // Requested but not received
    float download_rate;           // Measured rate (bytes/sec)

    // Estimated time for next request to arrive
    float queue_time() const {
        if (download_rate == 0) return INFINITY;
        return outstanding_bytes / download_rate;
    }
};
```

#### Apply to EZIO's Piece Picker

```cpp
// When requesting next block, prefer peers with shortest queue
std::vector<peer*> peers = get_available_peers(piece);

// Sort by queue time (ascending)
std::sort(peers.begin(), peers.end(), [](peer* a, peer* b) {
    return a->queue_time() < b->queue_time();
});

// Request from fastest peer
request_block(peers.front(), piece, block);
```

**Benefits:**
- More even distribution of requests
- Prevents slow peers from blocking piece completion
- Better network utilization

**Implementation Effort:** 2-3 days
**Priority:** Low (only if piece completion latency is issue)

---

## 13. Adaptive Timeouts (From streaming.rst)

### Current EZIO Timeout Strategy
- Unknown (needs investigation)

### libtorrent Approach
```cpp
// Maintain running average of piece completion time
struct timeout_tracker {
    float average_download_time;    // Mean time per piece
    float time_std_dev;              // Standard deviation

    float timeout_threshold() const {
        return average_download_time + (time_std_dev / 2.0f);
    }

    bool has_timed_out(piece_download& p) const {
        return p.elapsed_time() > timeout_threshold();
    }
};
```

**Benefits:**
- Adapts to network conditions
- Aggressive when network is fast
- Patient when network is slow
- Reduces redundant requests

**For EZIO:**
```cpp
// Track piece download times
std::deque<float> recent_piece_times;

void on_piece_complete(piece_index_t piece, duration elapsed) {
    recent_piece_times.push_back(elapsed.count());

    // Keep last 50 pieces
    if (recent_piece_times.size() > 50) {
        recent_piece_times.pop_front();
    }

    // Update timeout threshold
    update_timeout_threshold();
}

float calculate_timeout() {
    float mean = average(recent_piece_times);
    float stddev = std_deviation(recent_piece_times);

    // Timeout = mean + 1.5 * stddev
    // This catches 93% of normal cases
    return mean + 1.5f * stddev;
}
```

**Implementation Effort:** 2 days
**Priority:** Low (implement if request timeouts are observed)

---

## 14. Multi-Threaded Disk I/O Lessons (From hacking.rst)

### libtorrent Architecture

From `hacking.rst`, libtorrent uses:

1. **Network thread** - All socket I/O, peer state, piece picker
2. **Disk I/O threads** - Multiple threads for disk operations
3. **Hash threads** - Separate threads for SHA-1/SHA-256 computation

#### Thread Communication Pattern
```
Network Thread                  Disk Thread
    |                               |
    |--[disk_job: write_block]----->|
    |                               | (queue job)
    |                               | (process when ready)
    |                               | (call pwrite())
    |<--[completion_alert]---------|
    |                               |
```

### EZIO Current Architecture

Needs verification, but likely similar. Key question: **Are network and disk threads separate?**

### Recommendations

#### 14.1 Separate Network and Disk Threads
```cpp
class session_impl {
private:
    // Network thread (boost::asio io_context)
    boost::asio::io_context m_network_io;
    std::thread m_network_thread;

    // Disk I/O (separate thread pool)
    raw_disk_io m_disk_io;  // Has its own thread pools

public:
    void run() {
        // Start disk I/O threads
        m_disk_io.start();

        // Run network thread
        m_network_thread = std::thread([this] {
            m_network_io.run();
        });
    }
};
```

**Benefits:**
- Network never blocks on disk
- Disk never blocks on network
- Better CPU utilization

**Priority:** Verify current architecture first

#### 14.2 Lock-Free Job Queues
```cpp
// Use lock-free queue for disk jobs
#include <boost/lockfree/queue.hpp>

class raw_disk_io {
private:
    boost::lockfree::queue<write_job*> m_write_queue{1024};

public:
    void async_write(write_job* job) {
        // No lock needed!
        while (!m_write_queue.push(job)) {
            // Queue full, apply backpressure
            std::this_thread::sleep_for(1ms);
        }
    }
};
```

**Benefits:**
- Zero contention between threads
- Better scalability
- Lower latency

**Implementation Effort:** 2-3 days
**Priority:** Medium (after Phase 2)

---

## 15. Network Buffer Coalescing (From streaming.rst)

### Insight from libtorrent

From `streaming.rst`:

> "One optimization is to buffer all piece requests while looping over the time-
> critical pieces and not send them until one round is complete. This increases
> the chances that the request messages are coalesced into the same packet."

### Apply to EZIO

```cpp
class peer_connection {
private:
    std::vector<request_message> m_pending_requests;

public:
    void request_block(piece_index_t piece, block_index_t block) {
        // Don't send immediately
        m_pending_requests.push_back({piece, block});

        // Send when batch is full or timeout
        if (m_pending_requests.size() >= 10 || should_flush()) {
            flush_requests();
        }
    }

    void flush_requests() {
        // Build single message with all requests
        message msg = build_request_batch(m_pending_requests);

        // Send as single packet (TCP coalescing)
        async_write(msg);

        m_pending_requests.clear();
    }
};
```

**Benefits:**
- Fewer packets on network
- Lower latency (fewer round-trips)
- Reduced CPU overhead (fewer syscalls)

**Implementation Effort:** 1-2 days
**Priority:** Low (minor optimization)

---

## 16. File Pool for Partition Handles (From tuning.rst)

### Insight from libtorrent

From `tuning.rst` section "file pool":

> "libtorrent keeps an LRU cache for open file handles. Each file that is opened,
> is stuck in the cache. The main purpose of this is because of anti-virus
> software that hooks on file-open and file close to scan the file."

### Not Applicable to EZIO

EZIO operates on raw devices (`/dev/sda1`), not files, so:
- Only one "file" (block device) per storage
- No open/close overhead
- No file pool needed

**Verdict:** ✅ EZIO already optimal here

---

## 17. Peer Connection Limits (From tuning.rst)

### Status: ✅ Already Implemented

Connection limits are already configured in EZIO and tuned for production use.

**Settings available:**
```cpp
// From utils/ezio_add_torrent.py
max_connections = 3  (default)
max_uploads = 2      (default)
```

User has already found stable values through testing.

**This optimization is complete.** ✅

---

## 18. Rate-Based Choking (From manual.rst)

### What is Rate-Based Choking?

From `manual.rst`:

> "libtorrent supports a choking algorithm that automatically determines the number
> of upload slots (unchoke slots) based on the upload rate to peers."

### EZIO Use Case

**Not applicable for initial Clonezilla deployment** because:
- Seeder should unchoke ALL clients (cluster scenario)
- No need for tit-for-tat incentives
- Controlled environment

**Possible future use:**
- If EZIO expands to Internet-scale deployment
- If fairness/incentives become important

**Verdict:** ⏸️ Skip for now

---

## 19. uTP Protocol Considerations (From utp.rst)

### What is uTP?

uTP (Micro Transport Protocol) is a delay-based congestion control protocol used by libtorrent to avoid filling send buffers and causing latency.

### Key Insight for EZIO

From `utp.rst`:

> "uTP measures one-way delay and backs off when delay increases, preventing
> buffer bloat and keeping latency low for other applications."

### Should EZIO Use uTP?

**NO - Use TCP for EZIO**

**Reasons:**
1. **Controlled environment** - Clonezilla cluster is isolated network
2. **No competing traffic** - Imaging is sole purpose during deployment
3. **Want maximum throughput** - Don't need to be "nice" to other apps
4. **Simpler implementation** - TCP is built-in, well-tested
5. **Better tools** - More debugging/monitoring tools for TCP

**uTP Benefits Don't Apply:**
- Lower latency for web browsing → Not relevant in cluster
- Adaptive bandwidth → Want maximum always
- Firewall-friendly → LAN deployment

**Verdict:** ✅ Stick with TCP

---

## 20. Piece Picker Optimizations (From manual.rst and features.rst)

### libtorrent Piece Picker Features

From `manual.rst` and `features.rst`:

1. **Rarest first** - Download rarest pieces first (for swarming)
2. **Sequential download** - Download in order (for streaming)
3. **Random pick** - Initial mode for first pieces
4. **Reverse order** - For snubbed peers
5. **Parole mode** - Isolate corrupt peers
6. **Piece affinity** - Group peers by speed
7. **Prefer whole pieces** - Fast peers download complete pieces
8. **Prioritize partial** - Minimize partial pieces

### EZIO Context

EZIO has a **simpler scenario** than general BitTorrent:
- Single seeder initially (source machine)
- Sequential imaging (partition sectors in order)
- No need for complex piece picker

### Recommendations for EZIO

#### 20.1 Use Sequential Download Mode
```cpp
// EZIO should use sequential mode
torrent_params.flags |= torrent_flags::sequential_download;
```

**Reasons:**
- Disk imaging is inherently sequential
- Simpler than rarest-first
- Better for write coalescing (adjacent blocks)

#### 20.2 Prefer Whole Pieces
```cpp
// Set whole pieces threshold for fast peers
settings.set_int(settings_pack::whole_pieces_threshold, 10 * 1024 * 1024);
// 10 MB/s threshold
```

**Benefits:**
- Fast peers (10 Gbps) request entire pieces
- Better for write coalescing
- Reduces partial piece count

**Priority:** Medium
**Implementation Effort:** <1 day (configuration only)

---

## 21. Session Statistics & Profiling (From tuning.rst)

### libtorrent Profiling

From `tuning.rst`:

```cpp
// libtorrent exposes counters via session_stats_alert
ses.post_session_stats();  // Called periodically

// Parse with tools/parse_session_stats.py (generates graphs)
```

### Metrics Categories

1. **Counters** - Monotonically increasing (e.g., bytes_downloaded)
2. **Gauges** - Current state (e.g., num_peers_connected)

### Recommended Metrics for EZIO

```cpp
struct ezio_session_stats {
    // Throughput counters
    std::atomic<uint64_t> total_bytes_received{0};
    std::atomic<uint64_t> total_bytes_written{0};
    std::atomic<uint64_t> total_bytes_hashed{0};

    // Throughput gauges (computed per second)
    std::atomic<float> download_rate_mbps{0};
    std::atomic<float> write_rate_mbps{0};

    // Network gauges
    std::atomic<size_t> num_peers_connected{0};
    std::atomic<size_t> num_peers_unchoked{0};
    std::atomic<size_t> num_outstanding_requests{0};

    // Disk I/O gauges
    std::atomic<size_t> pending_write_jobs{0};
    std::atomic<size_t> pending_read_jobs{0};
    std::atomic<size_t> pending_hash_jobs{0};

    // Buffer pool gauges
    std::atomic<size_t> buffer_pool_size{0};
    std::atomic<size_t> buffer_pool_used{0};
    std::atomic<float> buffer_pool_utilization{0};  // %

    // Store buffer gauges
    std::atomic<size_t> store_buffer_blocks{0};
    std::atomic<size_t> store_buffer_bytes{0};

    // Write coalescing stats (Phase 2)
    std::atomic<uint64_t> coalesced_writes{0};
    std::atomic<uint64_t> single_block_writes{0};
    std::atomic<float> avg_coalesce_size{0};  // blocks per write

    // Latency histograms
    histogram write_latency_us;
    histogram read_latency_us;
    histogram hash_latency_us;

    // Export to log/prometheus/etc
    void log_stats() {
        spdlog::info("Stats: DL={:.2f} MB/s, WR={:.2f} MB/s, Peers={}, Buf={:.1f}%",
            download_rate_mbps,
            write_rate_mbps,
            num_peers_connected.load(),
            buffer_pool_utilization.load()
        );
    }
};

// Update periodically
void update_stats_thread() {
    while (running) {
        std::this_thread::sleep_for(1s);

        // Calculate rates
        stats.download_rate_mbps = calculate_rate(stats.total_bytes_received);
        stats.write_rate_mbps = calculate_rate(stats.total_bytes_written);
        stats.buffer_pool_utilization =
            100.0f * stats.buffer_pool_used / stats.buffer_pool_size;

        stats.log_stats();
    }
}
```

### Visualization

```bash
# Export to CSV for analysis
ezio --export-stats stats.csv

# Or integrate with Prometheus
# Grafana dashboard for real-time monitoring
```

**Priority:** HIGH (implement alongside Phase 2)
**Implementation Effort:** 2-3 days

---

## 22. Backpressure & Flow Control (Derived from tuning.rst)

### Problem: Disk Can't Keep Up

If network is 10 Gbps but disk is only 3 Gbps:
- Buffer pool fills up
- System runs out of memory
- OOM killer or crash

### libtorrent Approach

```cpp
// settings_pack::max_queued_disk_bytes
// Limits total bytes queued for disk I/O
```

### EZIO Implementation

Already partially implemented in buffer pool watermarks, but needs enhancement:

```cpp
class backpressure_controller {
private:
    size_t m_low_watermark;   // 50% - Normal operation
    size_t m_high_watermark;  // 87.5% - Start slowing down
    size_t m_critical;        // 95% - Emergency stop

public:
    enum class action {
        ACCEPT,      // Accept new data
        SLOW_DOWN,   // Accept but signal backpressure
        REJECT       // Reject new data (rare)
    };

    action check_buffer_state() {
        float usage = m_buffer_pool.utilization();

        if (usage < m_high_watermark) {
            return action::ACCEPT;
        } else if (usage < m_critical) {
            // Signal backpressure to peers
            // (reduce receive window, delay ACKs)
            return action::SLOW_DOWN;
        } else {
            // Emergency: reject new data
            return action::REJECT;
        }
    }

    void apply_backpressure() {
        // Reduce TCP receive window
        for (auto& peer : peers) {
            peer->set_recv_window(8192);  // Slow down sender
        }

        // Delay processing network events
        std::this_thread::sleep_for(10ms);
    }
};
```

**Priority:** High (prevents OOM crashes)
**Implementation Effort:** 1-2 days

---

## 23. Hash Verification Strategy (From features.rst)

### libtorrent Hash Verification

1. **Normal mode** - Download, then hash check
2. **Seed mode** - Assume correct, verify on first upload request

### EZIO Scenario

**Source machine (seeder):**
- Reading from disk first time
- Should verify hashes? Or trust source disk?

**Options:**

#### Option A: Trust Source Disk (Fast)
```cpp
// Skip hash verification on source
torrent_params.flags |= torrent_flags::seed_mode;
```
- **Pro:** Faster startup, no CPU overhead
- **Con:** If source disk has corruption, spreads to all clients

#### Option B: Verify Source (Safe)
```cpp
// Hash entire disk before serving
torrent_params.flags &= ~torrent_flags::seed_mode;
```
- **Pro:** Guarantees correctness
- **Con:** Initial hash check takes time (e.g., 1 TB @ 3 GB/s = 5 min)

### Recommendation

**Use Option B (verify) for safety:**
- Disk corruption is rare but catastrophic if undetected
- 5 minute initial check is acceptable for large-scale deployment
- Can parallelize with torrent creation

**Implementation:**
```cpp
// During torrent creation, compute hashes
auto torrent = create_torrent_from_partition("/dev/sda1",
    create_torrent_flags::CALCULATE_HASHES);

// Then seed with verified hashes
torrent_params.ti = std::make_shared<torrent_info>(torrent);
// seed_mode NOT set, so no reverification needed
```

**Priority:** Medium
**Implementation Effort:** Already done (if using libtorrent's hash calculation)

---

## 24. Piece Size Selection (From hacking.rst and manual.rst)

### libtorrent Defaults

- Typical: 512 KB to 4 MB pieces
- Trade-off: Smaller pieces = more overhead, larger pieces = slower verification

### EZIO Considerations

**Current:** Likely using libtorrent defaults

**Optimal for raw disk imaging:**

```
Disk Size       Recommended Piece Size    Total Pieces
--------        ----------------------    ------------
<  10 GB        1 MB                      10,000
10-100 GB       2 MB                      50,000
100-500 GB      4 MB                      125,000
500 GB - 2 TB   8 MB                      256,000
> 2 TB          16 MB                     128,000
```

**Rationale:**
- Larger pieces = fewer pieces = less overhead
- But not too large (slower hash verification)
- Target: 50k-250k pieces for good granularity

**For 1 TB disk:**
```cpp
size_t piece_size = 8 * 1024 * 1024;  // 8 MB
// Results in 128,000 pieces
```

**Benefits of 8 MB pieces:**
- 512 blocks per piece (16 KB block size)
- Good for write coalescing (can accumulate many blocks)
- Not too many pieces (reasonable memory)

**Priority:** Low (default is probably fine)
**Implementation Effort:** <1 day (configuration)

---

## 25. Error Handling & Retry Logic (From manual.rst)

### libtorrent Error Handling

From `manual.rst`:

> "Whenever a torrent encounters a fatal error, it will be stopped, and the
> torrent_status::error will describe the error that caused it."

> "If a torrent hits a disk write error, it will be put into upload mode."

### EZIO Needs Robust Error Handling

```cpp
class error_handler {
public:
    void handle_disk_error(storage_error const& error) {
        if (error.ec == boost::asio::error::no_space_on_device) {
            // Disk full - critical error
            spdlog::critical("Target disk full! Aborting.");
            abort_session();
        }
        else if (error.ec == boost::asio::error::io_error) {
            // I/O error - possibly bad sector
            spdlog::error("Disk I/O error at {}: {}", error.file, error.ec.message());

            // Retry policy
            if (should_retry(error)) {
                retry_operation(error);
            } else {
                mark_bad_sector(error);
            }
        }
    }

    bool should_retry(storage_error const& error) {
        // Retry transient errors
        return error.ec == boost::asio::error::device_busy
            || error.ec == boost::asio::error::interrupted;
    }

    void retry_operation(storage_error const& error) {
        // Exponential backoff
        std::this_thread::sleep_for(retry_delay);
        retry_delay *= 2;

        // Re-queue operation
        requeue_job(error);
    }
};
```

**Priority:** High (production robustness)
**Implementation Effort:** 2-3 days

---

## Updated Priority Matrix

| # | Optimization | Effort | Benefit | Priority | When |
|---|-------------|--------|---------|----------|------|
| 1 | Parallel Hashing | Medium | High | Medium | After Phase 2.2 |
| 2 | Adaptive Thread Pools | Low | Medium | Low | After stabilization |
| 3 | Memory Pool | High | Medium | Low | If malloc overhead > 5% |
| 4 | Zero-Copy Network | High | High | Medium | After Phase 2 |
| 5 | Direct I/O (O_DIRECT) | Medium | Negative | ❌ Never | Not recommended |
| 6 | io_uring | Very High | Very High | Low | Cutting edge only |
| 7 | Monitoring/Metrics | Low | Very High | **🔥 High** | Alongside Phase 2 |
| 8 | Settings Integration | Low | Medium | Medium | After Phase 1.2 |
| 9 | Storage Auto-Detect | Medium | Medium | Medium | After tuning stable |
| 10 | Page-Aligned Buffers | Low | Medium | Medium | Phase 1 |
| 11 | Socket Buffer Tuning | Low | High | **High** | Phase 1 |
| 12 | Request Queue Mgmt | Medium | Medium | Low | If latency issues |
| 13 | Adaptive Timeouts | Low | Medium | Low | If timeout issues |
| 14 | Lock-Free Queues | Medium | High | Medium | After Phase 2 |
| 15 | Network Buffer Coalescing | Low | Low | Low | Minor optimization |
| 16 | File Pool | N/A | N/A | ✅ N/A | Already optimal |
| 17 | Connection Limits | N/A | N/A | ✅ Done | Already configured |
| 18 | Rate-Based Choking | Medium | N/A | ⏸️ Skip | Not applicable |
| 19 | uTP Protocol | High | N/A | ❌ Skip | Stick with TCP |
| 20 | Piece Picker Config | Low | Medium | Medium | Phase 1 config |
| 21 | Session Statistics | Low | Very High | **🔥 High** | Phase 2 |
| 22 | Backpressure Control | Low | Very High | **🔥 High** | Phase 1 |
| 23 | Hash Verification | Low | High | Medium | Production safety |
| 24 | Piece Size Tuning | Low | Low | Low | Optional |
| 25 | Error Handling | Medium | Very High | **🔥 High** | Production robustness |

---

## Immediate Action Items (Before Phase 2 Deployment)

### Critical (Must Have)
1. ✅ **Monitoring & Metrics** (#7, #21) - Can't operate blind
2. ✅ **Backpressure Control** (#22) - Prevents OOM crashes
3. ✅ **Error Handling** (#25) - Production robustness

### High Priority (Should Have)
5. **Socket Buffer Tuning** (#11) - Simple, high impact
6. **Page-Aligned Buffers** (#10) - Foundation for future opts
7. **Hash Verification** (#23) - Correctness guarantee

### Medium Priority (Nice to Have)
8. **Piece Picker Config** (#20) - Sequential mode, whole pieces
9. **Settings Integration** (#8) - Runtime configuration
10. **Lock-Free Queues** (#14) - Better scalability

### Low Priority (Future Work)
- All others based on profiling results

---

## Rejected/Not Applicable Items

| # | Item | Reason |
|---|------|--------|
| 5 | Direct I/O (O_DIRECT) | ❌ Bad for sequential writes |
| 16 | File Pool | ✅ N/A for raw devices |
| 18 | Rate-Based Choking | ⏸️ Not needed for cluster |
| 19 | uTP Protocol | ❌ TCP is better for LAN |

---

## References

### Primary Sources
- `libtorrent-2.0.10/docs/tuning.rst` - Performance tuning guide
- `libtorrent-2.0.10/docs/manual.rst` - Architecture and design
- `libtorrent-2.0.10/docs/features.rst` - Feature list and capabilities
- `libtorrent-2.0.10/docs/streaming.rst` - Time-critical piece logic
- `libtorrent-2.0.10/docs/utp.rst` - uTP protocol details
- `libtorrent-2.0.10/docs/hacking.rst` - Internal architecture
- `libtorrent-2.0.10/include/libtorrent/settings_pack.hpp` - All settings

### Key Insights Applied
1. **Memory-mapped I/O** with store buffer architecture
2. **Page-aligned buffers** for optimal I/O
3. **Socket buffer sizing** based on BDP calculation
4. **Write coalescing** using writev()
5. **Adaptive timeouts** based on statistics
6. **Backpressure** to prevent buffer overflow
7. **Comprehensive metrics** for monitoring

---

**Document Version:** 2.0
**Last Updated:** 2024-12-14
**Status:** Comprehensive Analysis Complete
**Coverage:** All libtorrent 2.0.10 .rst documentation reviewed
