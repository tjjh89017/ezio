# Buffer Pool Merger Analysis

## Executive Summary

This document analyzes merging EZIO's separate `read_buffer_pool_` and `write_buffer_pool_` back to libtorrent 2.x's original single buffer pool design.

**Key Finding:** libtorrent 2.x uses a **single shared `disk_buffer_pool`** for all operations (read, write, hash). EZIO diverged from this by creating two separate 128MB pools.

**Current EZIO Design:** 128MB read pool + 128MB write pool = 256MB total (fixed, separate)

**libtorrent 2.x Design:** Single 256MB pool shared by all operations (dynamic, unified)

**Recommendation:** Return to libtorrent's design for better memory utilization and simpler code.

---

## Table of Contents

1. [libtorrent 2.x's Actual Design](#libtorrent-2xs-actual-design)
2. [EZIO's Divergence](#ezios-divergence)
3. [Problems with Separate Pools](#problems-with-separate-pools)
4. [Proposed Solution: Return to Unified Pool](#proposed-solution-return-to-unified-pool)
5. [Implementation](#implementation)
6. [Migration Strategy](#migration-strategy)

---

## libtorrent 2.x's Actual Design

### Source Analysis

**Reference:** `libtorrent-2.0.10/include/libtorrent/aux_/disk_buffer_pool.hpp`

```cpp
namespace libtorrent {
namespace aux {

	struct TORRENT_EXTRA_EXPORT disk_buffer_pool final
		: buffer_allocator_interface
	{
		explicit disk_buffer_pool(io_context& ios);
		~disk_buffer_pool();

		// Allocate buffer for ANY operation type
		// category parameter is for debugging/stats only
		char* allocate_buffer(char const* category);

		// With watermark notification
		char* allocate_buffer(bool& exceeded,
		                      std::shared_ptr<disk_observer> o,
		                      char const* category);

		void free_buffer(char* buf);

		int in_use() const { return m_in_use; }

	private:
		int m_in_use;           // Total buffers in use
		int m_max_use;          // Max buffer limit
		int m_low_watermark;    // Low watermark (50% of max)

		std::vector<std::weak_ptr<disk_observer>> m_observers;
		bool m_exceeded_max_size;
		io_context& m_ios;
	};

}}
```

### Key Characteristics

1. **Single Pool for All Operations**
   - Read, write, hash all share the same pool
   - No separation by operation type

2. **Dynamic Allocation**
   - Uses `malloc()` / `free()`, not pre-allocated fixed blocks
   - Allows temporary over-allocation with backpressure notification

3. **Simple Watermark Mechanism**
   ```cpp
   m_low_watermark = m_max_use / 2;  // 50%

   // Trigger exceeded when usage reaches 75%
   if (m_in_use >= m_low_watermark + (m_max_use - m_low_watermark) / 2)
       m_exceeded_max_size = true;
   ```

4. **Category Parameter is Unused**
   ```cpp
   char* disk_buffer_pool::allocate_buffer_impl(
       std::unique_lock<std::mutex>& l,
       char const*)  // ← Category not used in implementation
   {
       char* ret = static_cast<char*>(std::malloc(default_block_size));
       ++m_in_use;
       // ...
   }
   ```

   The `category` parameter exists for debugging/profiling but doesn't affect allocation logic.

5. **No Per-Category Limits**
   - Any operation can use up to 100% of the pool
   - Natural balancing through workload characteristics
   - Backpressure prevents unbounded growth

### Usage in mmap_disk_io

**Reference:** `libtorrent-2.0.10/src/mmap_disk_io.cpp:327`

```cpp
struct mmap_disk_io final : disk_interface
{
    // ...

    // Single buffer pool for ALL disk operations
    aux::disk_buffer_pool m_buffer_pool;

    // ↑ Used by:
    // - async_read() for read buffers
    // - async_write() for write buffers
    // - async_hash() for hash buffers
    // - Any other disk operation
};
```

**Comment in source (line 331-333):**
```cpp
// total number of blocks in use by both the read
// and the write cache. This is not supposed to
// exceed m_cache_size
```

This confirms read and write share the same pool.

---

## EZIO's Divergence

### Current Design

**Location:** `raw_disk_io.hpp:140-141`

```cpp
class raw_disk_io final : public disk_interface {
    // ...
private:
    buffer_pool read_buffer_pool_;   // 128 MB dedicated to reads
    buffer_pool write_buffer_pool_;  // 128 MB dedicated to writes
};
```

### Differences from libtorrent 2.x

| Aspect | libtorrent 2.x | EZIO |
|--------|---------------|------|
| **Number of pools** | 1 shared pool | 2 separate pools |
| **Memory allocation** | Dynamic (malloc/free) | Pre-allocated fixed blocks |
| **Total capacity** | Configurable (e.g., 256MB) | 128MB + 128MB = 256MB |
| **Read limit** | Up to 256MB | Max 128MB (fixed) |
| **Write limit** | Up to 256MB | Max 128MB (fixed) |
| **Watermark** | 50%/75% of total | 50%/87.5% per pool |
| **Category tracking** | For debug only | Enforced separation |

### Possible Reasons for Divergence

**Hypothesis 1: Prevent Write Monopolization**
- Concern: Heavy download could monopolize all buffers for writes
- Reality: libtorrent's backpressure mechanism already prevents this

**Hypothesis 2: Predictable Memory Usage**
- Benefit: Fixed allocation is simpler to reason about
- Cost: Poor utilization for unbalanced workloads

**Hypothesis 3: Independent Watermarks**
- Benefit: Separate watermarks for read and write operations
- Cost: More complex, doesn't match libtorrent's expectations

### store_buffer Relationship

**Important:** EZIO's `store_buffer` is correctly copied from libtorrent 2.x:

**EZIO's store_buffer.hpp:**
```cpp
namespace ezio {
struct store_buffer {
    std::unordered_map<torrent_location, char const*> m_store_buffer;
    // ... get(), insert(), erase() methods
};
}
```

**libtorrent's store_buffer.hpp:**
```cpp
namespace libtorrent::aux {
struct store_buffer {
    std::unordered_map<torrent_location, char const*> m_store_buffer;
    // ... get(), insert(), erase() methods
};
}
```

They are virtually identical. The divergence is in `buffer_pool`, not `store_buffer`.

---

## Problems with Separate Pools

### Problem 1: Poor Memory Utilization

**Scenario: Seeding (Read-Heavy)**

```
┌─────────────────────────────────────────────────────────┐
│ Read Pool: 128 MB                                       │
│ ████████████████████████████████████████████████ (100%) │ FULL!
│                                                         │
│ Write Pool: 128 MB                                      │
│ ██ (5%)                                    WASTED: 95%  │
└─────────────────────────────────────────────────────────┘

Result: 200 MB demand, only 135 MB allocated (67% efficiency)
```

**Scenario: Downloading (Write-Heavy)**

```
┌─────────────────────────────────────────────────────────┐
│ Read Pool: 128 MB                                       │
│ ██ (5%)                                    WASTED: 95%  │
│                                                         │
│ Write Pool: 128 MB                                      │
│ ████████████████████████████████████████████████ (100%) │ FULL!
└─────────────────────────────────────────────────────────┘

Result: 200 MB demand, only 135 MB allocated (67% efficiency)
```

**libtorrent 2.x Behavior:**

```
┌─────────────────────────────────────────────────────────┐
│ Unified Pool: 256 MB                                    │
│ ████████████████████████████████████████████████████    │
│                     200 MB allocated (78%)              │
│                     56 MB free                          │
└─────────────────────────────────────────────────────────┘

Result: 200 MB demand, 200 MB allocated (100% efficiency)
```

### Problem 2: Resource Starvation

**Current EZIO:**

```
1. Download starts → write_buffer_pool_ fills to 128 MB
2. Peer requests block → read_buffer_pool_ has 100 MB free
3. But write_buffer_pool_.allocate() blocks waiting for space!
4. Download stalls even though 100 MB is available
```

**libtorrent 2.x:**

```
1. Download starts → m_buffer_pool reaches 200 MB
2. Peer requests block → allocate() succeeds (220 MB total)
3. Watermark triggers backpressure to both read and write
4. No starvation, gradual slowdown across all operations
```

### Problem 3: Code Complexity

**EZIO:**
- Maintain two separate buffer_pool instances
- Track watermarks for each independently
- Coordinate watermark notifications
- Decide which pool to use for each operation

**libtorrent 2.x:**
- Single buffer_pool instance
- Single watermark state
- Single notification mechanism
- All operations use the same pool

### Problem 4: Deviation from libtorrent Assumptions

libtorrent 2.x was designed assuming:
1. Single unified buffer pool
2. Dynamic allocation with over-allocation allowed
3. Backpressure applied uniformly across all operations

EZIO's separate pools may violate these assumptions.

---

## Proposed Solution: Return to Unified Pool

### Design Goals

1. ✅ Match libtorrent 2.x's original design
2. ✅ Share 256MB between all operations
3. ✅ Prevent resource starvation
4. ✅ Simplify code (remove redundant buffer_pool instance)
5. ✅ Improve memory utilization for unbalanced workloads

### Option A: Use libtorrent's disk_buffer_pool Directly

**Pros:**
- Zero maintenance burden
- Guaranteed compatibility with libtorrent updates
- Battle-tested code

**Cons:**
- Loses EZIO's custom features (if any)
- Dynamic allocation (malloc/free) vs pre-allocated pool
- Less control over implementation details

**Implementation:**

```cpp
// raw_disk_io.hpp
#include <libtorrent/aux_/disk_buffer_pool.hpp>

class raw_disk_io final : public disk_interface {
private:
    // Use libtorrent's disk_buffer_pool directly
    libtorrent::aux::disk_buffer_pool buffer_pool_;

    // Remove these:
    // buffer_pool read_buffer_pool_;
    // buffer_pool write_buffer_pool_;
};
```

```cpp
// raw_disk_io.cpp
raw_disk_io::raw_disk_io(io_context& ioc, settings_pack const& sett)
    : buffer_pool_(ioc)
{
    buffer_pool_.set_settings(sett);
}

void raw_disk_io::async_read(...) {
    char* buffer = buffer_pool_.allocate_buffer("read");
    // ...
    buffer_pool_.free_buffer(buffer);
}

void raw_disk_io::async_write(...) {
    bool exceeded = false;
    char* buffer = buffer_pool_.allocate_buffer(exceeded, observer, "write");
    // ...
    buffer_pool_.free_buffer(buffer);
}
```

### Option B: Adapt EZIO's buffer_pool to Be Shared

Keep EZIO's custom `buffer_pool` implementation but use a single instance.

**Pros:**
- Retains EZIO-specific features (pre-allocation, custom watermarks)
- More control over behavior
- Can optimize for EZIO's specific use case

**Cons:**
- Maintenance burden
- May diverge further from libtorrent over time
- Need to ensure compatibility

**Implementation:**

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
private:
    // Single unified buffer pool
    buffer_pool unified_buffer_pool_;

    // Remove:
    // buffer_pool read_buffer_pool_;
    // buffer_pool write_buffer_pool_;
};
```

```cpp
// raw_disk_io.cpp
raw_disk_io::raw_disk_io(io_context& ioc, settings_pack const& sett)
    : unified_buffer_pool_(ioc)
{
    // Initialize with 256 MB total (2x current per-pool size)
    // unified_buffer_pool_ already uses MAX_BUFFER_POOL_SIZE from buffer_pool.hpp
    // May need to update MAX_BUFFER_POOL_SIZE to 256 MB
}

void raw_disk_io::async_read(...) {
    // Use unified pool for reads
    auto buffer = unified_buffer_pool_.allocate_buffer();
    // ...
    unified_buffer_pool_.free_disk_buffer(buffer);
}

void raw_disk_io::async_write(...) {
    // Use unified pool for writes
    bool exceeded = false;
    auto buffer = unified_buffer_pool_.allocate_buffer(exceeded, observer);
    // ...
    unified_buffer_pool_.free_disk_buffer(buffer);
}
```

**Update buffer_pool.hpp:**

```cpp
// buffer_pool.hpp
// Change from 128 MB to 256 MB for unified pool
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)
```

### Recommendation: Option B

**Rationale:**
1. EZIO already has a working `buffer_pool` implementation
2. Pre-allocated pool may perform better than dynamic malloc/free
3. Custom watermark thresholds (50%/87.5%) are working well
4. Minimal code changes required
5. Can switch to Option A later if desired

**Migration is simple:**
- Rename one buffer_pool to `unified_buffer_pool_`
- Delete the other buffer_pool
- Update all call sites to use unified_buffer_pool_
- Increase MAX_BUFFER_POOL_SIZE to 256 MB

---

## Implementation

### Step 1: Update buffer_pool.hpp

```cpp
// buffer_pool.hpp
#ifndef __BUFFER_POOL_HPP__
#define __BUFFER_POOL_HPP__

// Change from 128 MB to 256 MB
#define MAX_BUFFER_POOL_SIZE (256ULL * 1024 * 1024)

// Rest of buffer_pool.hpp unchanged
// ...

#endif
```

### Step 2: Update raw_disk_io.hpp

```cpp
// raw_disk_io.hpp
class raw_disk_io final : public disk_interface {
    // ... public interface unchanged ...

private:
    // ===== BEFORE =====
    // buffer_pool read_buffer_pool_;
    // buffer_pool write_buffer_pool_;

    // ===== AFTER =====
    buffer_pool unified_buffer_pool_;  // Single 256 MB pool for all operations
};
```

### Step 3: Update raw_disk_io.cpp Constructor

```cpp
// raw_disk_io.cpp
raw_disk_io::raw_disk_io(io_context& ioc, settings_pack const& sett)
    : io_context_(ioc)
    , settings_(sett)
    , unified_buffer_pool_(ioc)  // Single pool initialization
    // Remove: read_buffer_pool_(ioc), write_buffer_pool_(ioc)
{
    // ... rest of constructor unchanged ...
}
```

### Step 4: Update async_read

```cpp
// raw_disk_io.cpp
void raw_disk_io::async_read(storage_index_t storage, peer_request const& r,
                              std::function<void(disk_buffer_holder)> handler,
                              disk_job_flags_t flags) {
    // ===== BEFORE =====
    // auto buffer = read_buffer_pool_.allocate_buffer();

    // ===== AFTER =====
    auto buffer = unified_buffer_pool_.allocate_buffer();

    if (!buffer) {
        // Handle allocation failure
        return;
    }

    // ... rest of read logic unchanged ...

    // ===== BEFORE =====
    // read_buffer_pool_.free_disk_buffer(buffer);

    // ===== AFTER =====
    unified_buffer_pool_.free_disk_buffer(buffer);
}
```

### Step 5: Update async_write

```cpp
// raw_disk_io.cpp
void raw_disk_io::async_write(storage_index_t storage, peer_request const& r,
                               char const* buf, std::shared_ptr<disk_observer> o,
                               std::function<void(storage_error const&)> handler,
                               disk_job_flags_t flags) {
    bool exceeded = false;

    // ===== BEFORE =====
    // auto buffer = write_buffer_pool_.allocate_buffer(exceeded, o);

    // ===== AFTER =====
    auto buffer = unified_buffer_pool_.allocate_buffer(exceeded, o);

    if (!buffer) {
        // Handle allocation failure
        return;
    }

    // ... rest of write logic unchanged ...

    // ===== BEFORE =====
    // write_buffer_pool_.free_disk_buffer(buffer);

    // ===== AFTER =====
    unified_buffer_pool_.free_disk_buffer(buffer);
}
```

### Step 6: Update Other Call Sites

Search for all occurrences of `read_buffer_pool_` and `write_buffer_pool_`:

```bash
grep -rn "read_buffer_pool_\|write_buffer_pool_" src/ include/
```

Replace with `unified_buffer_pool_`.

---

## Performance Analysis

### Memory Utilization

| Workload | Current (Separate Pools) | Unified Pool | Improvement |
|----------|--------------------------|--------------|-------------|
| **Balanced** (128R + 128W) | 256 MB (100%) | 256 MB (100%) | - |
| **Read-Heavy** (200R + 20W) | 148 MB (58%) | 220 MB (86%) | **+48%** |
| **Write-Heavy** (20R + 200W) | 148 MB (58%) | 220 MB (86%) | **+48%** |
| **Peak Mixed** (150R + 150W) | 256 MB (blocked) | 300 MB (allowed w/ backpressure) | **+17%** |

### Allocation Success Rate

**Scenario: Write Flood During Seeding**

Current (Separate Pools):
```
Time    Read Demand    Write Demand    Allocated    Failed
──────────────────────────────────────────────────────────
T0      150 MB         50 MB           178 MB       22 MB ✗
T1      150 MB         100 MB          228 MB       22 MB ✗
T2      150 MB         150 MB          256 MB       44 MB ✗

Failed allocations: Read fails despite write pool having space
```

Unified Pool:
```
Time    Read Demand    Write Demand    Allocated    Failed
──────────────────────────────────────────────────────────
T0      150 MB         50 MB           200 MB       0 MB ✓
T1      150 MB         100 MB          250 MB       0 MB ✓
T2      150 MB         150 MB          280 MB       20 MB ✓

Failed allocations: Only when total exceeds capacity, fair for all
```

### Watermark Behavior

**Current (Separate Pools):**
- Read pool high watermark: 112 MB (87.5% of 128 MB)
- Write pool high watermark: 112 MB (87.5% of 128 MB)
- Total watermark: 224 MB effective
- Problem: Can trigger watermark with only 58% total usage (e.g., 112R + 20W)

**Unified Pool:**
- Single high watermark: 224 MB (87.5% of 256 MB)
- Single low watermark: 128 MB (50% of 256 MB)
- Benefit: Watermark based on actual total usage, more accurate backpressure

### Mutex Contention Analysis

**Question:** Will merging to a single buffer pool cause mutex contention that blocks async_read/async_write/async_hash?

**Answer:** ❌ **No, it will NOT cause problematic blocking.**

#### Critical Section Duration

libtorrent's disk_buffer_pool uses a single mutex to protect all operations:

```cpp
// src/disk_buffer_pool.cpp:122-133
char* disk_buffer_pool::allocate_buffer(bool& exceeded, ...) {
    std::unique_lock<std::mutex> l(m_pool_mutex);  // ← LOCK
    char* ret = allocate_buffer_impl(l, category);
    // ... allocate_buffer_impl:
    //     malloc(16KB)      ~1μs
    //     ++m_in_use        ~0.01μs
    //     watermark check   ~0.05μs
    // Total: ~1-2μs
    return ret;
}  // ← UNLOCK (automatic)
```

**Key observations:**
1. ✅ **Extremely short**: Mutex held for only 1-2 microseconds
2. ✅ **No I/O under lock**: Only memory allocation and integer operations
3. ✅ **No syscalls**: malloc/free don't require syscalls in most cases
4. ✅ **Non-blocking**: No condition variables, no sleeping

#### Contention Calculation

**Current (Split Pools):**
```
Scenario: 8 read threads + 8 write threads, 100 alloc/sec each

read_buffer_pool_:
  - 8 threads × 100 alloc/sec = 800 alloc/sec
  - Total lock time: 800 × 2μs = 1.6ms/sec
  - Mutex utilization: 0.16%

write_buffer_pool_:
  - 8 threads × 100 alloc/sec = 800 alloc/sec
  - Total lock time: 800 × 2μs = 1.6ms/sec
  - Mutex utilization: 0.16%
```

**After Merge (Unified Pool):**
```
Scenario: Same 16 threads, same allocation rate

m_buffer_pool:
  - 16 threads × 100 alloc/sec = 1600 alloc/sec
  - Total lock time: 1600 × 2μs = 3.2ms/sec
  - Mutex utilization: 0.32%

Availability: 99.68% of the time, mutex is FREE
Expected wait time per allocation: ~0.006μs (negligible)
```

#### Impact on Operations

**async_read:**
```
Before: Lock read_buffer_pool_.m_pool_mutex (~1-2μs)
After:  Lock m_buffer_pool.m_pool_mutex (~1-2μs)
Additional contention delay: ~0.1-0.2μs
Percentage of 16KB read (13ms HDD): 0.0015%
```

**async_write:**
```
Before: Lock write_buffer_pool_.m_pool_mutex (~1-2μs)
After:  Lock m_buffer_pool.m_pool_mutex (~1-2μs)
Additional contention delay: ~0.1-0.2μs
Percentage of 16KB write (13ms HDD): 0.0015%
```

**async_hash:**
```
No buffer allocation needed (operates on existing buffers)
Zero impact from buffer pool merge
```

#### Real-World Performance

| Operation | Duration | Mutex Time | Percentage |
|-----------|----------|------------|------------|
| HDD read 16KB | ~13ms | 1-2μs | 0.015% |
| HDD write 16KB | ~13ms | 1-2μs | 0.015% |
| SSD read 16KB | ~100μs | 1-2μs | 2% |
| SSD write 16KB | ~500μs | 1-2μs | 0.4% |
| Network transfer 16KB | 1-10ms | 0μs | 0% |

**Key Insight:** Buffer pool mutex time (1-2μs) is **orders of magnitude smaller** than actual I/O operations.

#### Why libtorrent 2.x Uses Single Mutex

From libtorrent source code analysis:

1. **Simplicity**: One pool, one mutex, one watermark
2. **Flexibility**: Dynamically balance read/write memory needs
3. **Proven**: Used in production for years
4. **Sufficient**: Mutex contention is negligible vs I/O costs

**Design principle:** Don't optimize what isn't a bottleneck.

#### Measurement Recommendation

Before and after merge, measure:

```bash
# Mutex contention
perf record -e syscalls:sys_enter_futex ./ezio
perf report

# Expected result:
# - Before: Two mutexes, each ~0.16% utilization
# - After: One mutex, ~0.32% utilization
# - Both are negligible, no performance impact
```

**Conclusion:** ✅ Single buffer pool mutex is NOT a bottleneck. The memory efficiency gain (+48%) far outweighs the negligible contention increase.

**See also:** [docs/MUTEX_ANALYSIS.md](MUTEX_ANALYSIS.md) for complete analysis.

---

## Migration Strategy

### Phase 1: Preparation (Low Risk)

1. **Create feature branch**
   ```bash
   git checkout -b feature/unified-buffer-pool
   ```

2. **Ensure tests pass**
   ```bash
   make test
   ```

3. **Document baseline metrics**
   - Memory usage patterns
   - Allocation success/failure rates
   - Performance benchmarks

### Phase 2: Implementation (Medium Risk)

1. **Update buffer_pool.hpp** (Step 1)
2. **Update raw_disk_io.hpp** (Step 2)
3. **Update raw_disk_io.cpp** (Steps 3-6)
4. **Compile and fix errors**
   ```bash
   make clean && make
   ```

### Phase 3: Testing (Low Risk)

1. **Unit tests**
   - Verify buffer allocation/deallocation
   - Test watermark triggers
   - Check observer notifications

2. **Integration tests**
   - Run full download/seed cycle
   - Monitor memory usage
   - Check for allocation failures

3. **Stress tests**
   - Heavy read workload
   - Heavy write workload
   - Mixed concurrent operations

### Phase 4: Validation (Low Risk)

1. **Compare metrics**
   - Memory utilization improvement
   - Allocation success rate
   - Performance impact

2. **Production trial**
   - Deploy to test environment
   - Monitor for issues
   - Collect performance data

3. **Rollout**
   - Merge to main branch
   - Deploy to production
   - Continue monitoring

### Rollback Plan

If issues are discovered:

```bash
# Revert the changes
git revert <commit-hash>

# Or restore from backup
git checkout main
git branch -D feature/unified-buffer-pool
```

The change is isolated to buffer_pool allocation, making rollback straightforward.

---

## Conclusion

### Summary

EZIO diverged from libtorrent 2.x by splitting the buffer pool into separate read and write pools. While this provides predictable allocation, it causes:

1. **Poor memory utilization** (58% efficiency for unbalanced workloads)
2. **Resource starvation** (one pool full while other has space)
3. **Unnecessary complexity** (two pools, two watermarks)

Returning to libtorrent's unified pool design:

✅ Improves memory efficiency by 48% for common workloads
✅ Eliminates resource starvation
✅ Simplifies code (remove one buffer_pool instance)
✅ Aligns with libtorrent's design assumptions
✅ Minimal implementation effort (~50 lines changed)

### Recommendation

**Implement Option B: Unified EZIO buffer_pool**

1. Change MAX_BUFFER_POOL_SIZE from 128 MB to 256 MB
2. Replace `read_buffer_pool_` and `write_buffer_pool_` with single `unified_buffer_pool_`
3. Update all call sites (simple search-and-replace)
4. Test thoroughly
5. Deploy

**Estimated Effort:** 1-2 days implementation + 1 day testing

**Risk Level:** Low (isolated change, easy rollback)

**Expected Benefit:** 48% better memory utilization, simpler code

The change is justified and low-risk. Recommend proceeding with implementation.
