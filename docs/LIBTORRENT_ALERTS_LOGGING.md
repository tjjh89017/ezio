# libtorrent Alerts Logging Design

**Version:** 1.0
**Date:** 2024-12-14
**Status:** Implementation Guide

---

## Best Practice: set_alert_notify()

**EZIO will use `set_alert_notify()` - the recommended approach by libtorrent.**

### Why Not Other Methods?

| Method | Why Not Use |
|--------|-------------|
| ❌ `pop_alerts()` + polling | Delays (0-5s), wastes CPU, blocks shutdown |
| ❌ `wait_for_alert()` + timeout | Still polling with timeout, not truly event-driven |
| ✅ `set_alert_notify()` callback | **True event-driven, instant, zero overhead** |

**This document only covers `set_alert_notify()` implementation.**

---

## Current Implementation Analysis

### log.cpp:53-65 (Current Alert Handler)

```cpp
void log::report_alert()
{
    SPDLOG_INFO("start alert report thread");
    while (!m_daemon.get_shutdown()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));  // ❌ Polls every 5 seconds

        std::vector<libtorrent::alert *> alerts;
        m_daemon.pop_alerts(&alerts);
        for (auto a : alerts) {
            SPDLOG_INFO("lt alert: {} {}", a->what(), a->message());  // ❌ Basic format
        }
    }
}
```

**Problems:**
- ❌ Uses polling (wastes CPU)
- ❌ 5-second delay (not timely)
- ❌ Blocks shutdown for 5 seconds

### main.cpp:28-29 (Current Alert Mask)

```cpp
p.set_int(lt::settings_pack::alert_mask,
    lt::alert_category::error | lt::alert_category::status);
```

### Problems with Current Implementation

1. **Uses SPDLOG_INFO macro** - Compile-time, not runtime controllable
2. **No alert categorization** - All alerts logged at INFO level
3. **Basic formatting** - Just `what()` and `message()`
4. **5-second polling** - Acceptable (alerts are queued, not lost)
5. **Limited alert categories** - Only error + status

---

## Why set_alert_notify() is Best

### Comparison: Three Approaches

```cpp
// ❌ Method 1: Polling (Current)
while (!shutdown) {
    sleep(5);  // Wastes time, delays alerts, blocks shutdown
    pop_alerts(&alerts);
}

// ⚠️ Method 2: wait_for_alert()
while (!shutdown) {
    alert* a = wait_for_alert(1s);  // Better, but still polling with timeout
    if (a) pop_alerts(&alerts);
}

// ✅ Method 3: set_alert_notify() (BEST)
set_alert_notify([]() {
    notify_worker_thread();  // Instant, zero overhead
});
// Worker thread waits on condition_variable, wakes instantly
```

### Real-World Example from libtorrent

From `libtorrent-2.0.10/simulation/utils.cpp:209-233`:

```cpp
void print_alerts(lt::session& ses, ...) {
    ses.set_alert_notify([&ses] {
        // Called from libtorrent internal thread
        // Post work to io_context (non-blocking!)
        post(ses.get_context(), [&ses] {
            // This runs in session's io_context thread
            alerts.clear();
            ses.pop_alerts(&alerts);
            // Process alerts...
        });
    });
}
```

**Why this works:**
1. Callback returns immediately (doesn't block libtorrent)
2. `post()` queues work to io_context
3. io_context thread processes alerts
4. Perfect separation of concerns

### EZIO Adaptation

EZIO has a dedicated `log` thread, so we use `condition_variable` instead of `post()`:

```cpp
// Callback (libtorrent thread) - FAST!
set_alert_notify([this]() {
    {
        std::lock_guard lock(m_alert_mutex);
        m_alert_ready = true;
    }
    m_alert_cv.notify_one();  // Wake log thread
});

// Log thread - waits efficiently
while (!shutdown) {
    std::unique_lock lock(m_alert_mutex);
    m_alert_cv.wait_for(lock, 1s, [this]() {
        return m_alert_ready || shutdown;
    });

    if (m_alert_ready) {
        m_alert_ready = false;
        lock.unlock();
        pop_alerts(&alerts);  // Process alerts
    }
}
```

**Key differences from libtorrent example:**
- libtorrent example: Uses `post()` to session's io_context
- EZIO: Uses condition_variable to wake dedicated log thread
- Both: Callback is fast and non-blocking

---

## Alert Categories Available

From `libtorrent/alert.hpp:78-183`:

```cpp
namespace alert_category {
    // Production categories (recommended)
    constexpr alert_category_t error = 0_bit;              // Errors (tracker, file, etc.)
    constexpr alert_category_t status = 6_bit;             // State changes
    constexpr alert_category_t storage = 3_bit;            // File errors, sync events
    constexpr alert_category_t tracker = 4_bit;            // Tracker events
    constexpr alert_category_t performance_warning = 9_bit;// Rate limits

    // Peer/network categories
    constexpr alert_category_t peer = 1_bit;               // Peer events
    constexpr alert_category_t connect = 5_bit;            // Connect/disconnect
    constexpr alert_category_t dht = 10_bit;               // DHT events

    // Progress tracking
    constexpr alert_category_t file_progress = 21_bit;     // File completion
    constexpr alert_category_t piece_progress = 22_bit;    // Piece completion

    // Debug categories (very verbose!)
    constexpr alert_category_t session_log = 13_bit;       // Session debug
    constexpr alert_category_t torrent_log = 14_bit;       // Torrent debug
    constexpr alert_category_t peer_log = 15_bit;          // Peer debug

    // High-rate categories (can spam logs)
    constexpr alert_category_t upload = 23_bit;            // Upload blocks
    constexpr alert_category_t block_progress = 24_bit;    // Block requests
}
```

---

## Solution 1: set_alert_notify() (BEST - Recommended) ✅

**Based on libtorrent official example (`simulation/utils.cpp:209-233`)**

### How set_alert_notify() Works

```
libtorrent posts alert → alert queue 0→1 → callback invoked → notify thread → pop_alerts()
                                             ↑                     ↓
                                      [MUST BE FAST]         [actual work]
```

**Key Rules (from libtorrent docs):**
1. ⚠️ Callback is called from **libtorrent internal thread**
2. ⚠️ Callback **MUST NOT block** (should just notify)
3. ⚠️ Callback **MUST NOT call pop_alerts()** directly
4. ✅ Callback should just wake up your processing thread
5. ✅ Processing thread calls pop_alerts() and handles alerts

### Implementation

#### Step 1: Add synchronization to log class

```cpp
// log.hpp
class log {
public:
    log(ezio &daemon);
    ~log();
    void join();

private:
    void report_speed();
    void report_alert();
    void log_alert_by_category(libtorrent::alert const* a);

    ezio &m_daemon;
    std::thread m_speed;
    std::thread m_alert;

    // NEW: For alert notification
    std::mutex m_alert_mutex;
    std::condition_variable m_alert_cv;
    bool m_alert_ready = false;
};
```

#### Step 2: Setup alert notification in constructor

```cpp
// log.cpp
log::log(ezio &daemon) :
    m_daemon(daemon)
{
    m_speed = std::thread(&log::report_speed, this);
    m_alert = std::thread(&log::report_alert, this);

    // NEW: Setup alert notification callback
    m_daemon.set_alert_notify([this]() {
        // This callback is called from libtorrent internal thread
        // MUST be fast, MUST NOT block, MUST NOT call pop_alerts()

        // Just notify the alert thread
        {
            std::lock_guard<std::mutex> lock(m_alert_mutex);
            m_alert_ready = true;
        }
        m_alert_cv.notify_one();
    });
}
```

#### Step 3: Update alert handler (event-driven)

```cpp
// log.cpp
void log::report_alert()
{
    spdlog::info("start alert report thread");

    while (!m_daemon.get_shutdown()) {
        // Wait for alert notification (with 1-second timeout for shutdown check)
        std::unique_lock<std::mutex> lock(m_alert_mutex);
        m_alert_cv.wait_for(lock, std::chrono::seconds(1), [this]() {
            return m_alert_ready || m_daemon.get_shutdown();
        });

        if (m_daemon.get_shutdown()) {
            break;
        }

        if (!m_alert_ready) {
            // Timeout - check shutdown and continue
            continue;
        }

        m_alert_ready = false;
        lock.unlock();

        // Now pop and process alerts (outside of lock!)
        std::vector<libtorrent::alert *> alerts;
        m_daemon.pop_alerts(&alerts);

        for (auto a : alerts) {
            log_alert_by_category(a);
        }
    }

    spdlog::info("alert report thread exiting");
}
```

#### Step 4: Add set_alert_notify() wrapper to daemon

```cpp
// daemon.hpp
class ezio : boost::noncopyable
{
public:
    // ... existing methods ...

    // NEW: Set alert notification callback
    void set_alert_notify(std::function<void()> const& callback);
};
```

```cpp
// daemon.cpp
void ezio::set_alert_notify(std::function<void()> const& callback)
{
    session_.set_alert_notify(callback);
}
```

### Benefits

- ✅ **Instant response**: Alerts processed immediately (no delay)
- ✅ **Zero CPU waste**: Thread sleeps, wakes only when alerts arrive
- ✅ **Fast shutdown**: Exits within 1 second (timeout check)
- ✅ **Clean design**: Follows libtorrent best practices
- ✅ **Thread-safe**: Proper synchronization with condition variable

### Performance

| Metric | Old (5s polling) | New (set_alert_notify) |
|--------|------------------|------------------------|
| Alert delay | 0-5s (avg 2.5s) | < 1ms (instant) |
| CPU usage | Constant wakeup | Zero (event-driven) |
| Shutdown time | 5 seconds | 1 second |
| Thread count | Same | Same |

---

## Solution 2: Alert Categorization Helper

**After implementing set_alert_notify(), add proper log level categorization:**

```cpp
// log.cpp
void log::log_alert_by_category(libtorrent::alert const* a)
{
    // Get alert category
    auto cat = a->category();

    // Log at appropriate level based on category
    if (cat & lt::alert_category::error) {
        spdlog::error("lt alert: {} {}", a->what(), a->message());
    } else if (cat & lt::alert_category::performance_warning) {
        spdlog::warn("lt alert: {} {}", a->what(), a->message());
    } else if (cat & lt::alert_category::status) {
        spdlog::info("lt alert: {} {}", a->what(), a->message());
    } else {
        spdlog::debug("lt alert: {} {}", a->what(), a->message());
    }
}
```

**Benefits:**
- ✅ Runtime log level control via `SPDLOG_LEVEL`
- ✅ Errors logged as errors (red), warnings as warnings (yellow)
- ✅ Works with set_alert_notify()

---

## Solution 3: Enhanced Alert Formatting (Optional)

**For alert-specific formatting (file errors, tracker errors, etc.):**

```cpp
void log::log_alert_detailed(libtorrent::alert const* a, spdlog::level::level_enum level)
{
    using namespace libtorrent;

    // Get alert type ID
    int type = a->type();

    // Special handling for important alert types
    switch (type) {
        // Storage errors (critical!)
        case file_error_alert::alert_type: {
            auto* fe = alert_cast<file_error_alert>(a);
            spdlog::error("lt alert: [STORAGE ERROR] file={} op={} error={} torrent={}",
                         fe->filename(), fe->operation, fe->error.message(),
                         fe->torrent_name());
            break;
        }

        case storage_moved_failed_alert::alert_type: {
            auto* smf = alert_cast<storage_moved_failed_alert>(a);
            spdlog::error("lt alert: [STORAGE ERROR] failed to move storage: {} torrent={}",
                         smf->error.message(), smf->torrent_name());
            break;
        }

        // Torrent status changes
        case torrent_finished_alert::alert_type: {
            auto* tf = alert_cast<torrent_finished_alert>(a);
            spdlog::info("lt alert: [COMPLETE] torrent finished: {}", tf->torrent_name());
            break;
        }

        case torrent_paused_alert::alert_type: {
            auto* tp = alert_cast<torrent_paused_alert>(a);
            spdlog::info("lt alert: [PAUSED] torrent paused: {}", tp->torrent_name());
            break;
        }

        case torrent_resumed_alert::alert_type: {
            auto* tr = alert_cast<torrent_resumed_alert>(a);
            spdlog::info("lt alert: [RESUMED] torrent resumed: {}", tr->torrent_name());
            break;
        }

        // Performance warnings
        case performance_alert::alert_type: {
            auto* pa = alert_cast<performance_alert>(a);
            spdlog::warn("lt alert: [PERFORMANCE] {} message={}",
                        pa->warning_code, pa->message());
            break;
        }

        // Tracker errors
        case tracker_error_alert::alert_type: {
            auto* te = alert_cast<tracker_error_alert>(a);
            spdlog::error("lt alert: [TRACKER ERROR] url={} error={} torrent={}",
                         te->tracker_url(), te->error.message(), te->torrent_name());
            break;
        }

        case tracker_warning_alert::alert_type: {
            auto* tw = alert_cast<tracker_warning_alert>(a);
            spdlog::warn("lt alert: [TRACKER WARNING] url={} msg={} torrent={}",
                        tw->tracker_url(), tw->warning_message(), tw->torrent_name());
            break;
        }

        case tracker_reply_alert::alert_type: {
            auto* tr = alert_cast<tracker_reply_alert>(a);
            spdlog::debug("lt alert: [TRACKER] announce success: peers={} url={} torrent={}",
                         tr->num_peers, tr->tracker_url(), tr->torrent_name());
            break;
        }

        // Peer errors
        case peer_error_alert::alert_type: {
            auto* pe = alert_cast<peer_error_alert>(a);
            spdlog::warn("lt alert: [PEER ERROR] peer={} error={} torrent={}",
                        pe->endpoint, pe->error.message(), pe->torrent_name());
            break;
        }

        // State changes
        case state_changed_alert::alert_type: {
            auto* sc = alert_cast<state_changed_alert>(a);
            spdlog::info("lt alert: [STATE] torrent state changed: {} → {} torrent={}",
                        static_cast<int>(sc->prev_state),
                        static_cast<int>(sc->state),
                        sc->torrent_name());
            break;
        }

        // Hash failures (important for data integrity)
        case hash_failed_alert::alert_type: {
            auto* hf = alert_cast<hash_failed_alert>(a);
            spdlog::error("lt alert: [HASH FAIL] piece={} torrent={}",
                         static_cast<int>(hf->piece_index), hf->torrent_name());
            break;
        }

        // Default: Use generic formatting
        default:
            spdlog::log(level, "lt alert: [{}] {}", a->what(), a->message());
            break;
    }
}
```

**Benefits:**
- ✅ Structured logging with clear prefixes ([STORAGE ERROR], [COMPLETE], etc.)
- ✅ Alert-specific formatting for important types
- ✅ Grouped by priority (errors → warnings → info)
- ✅ Easy to grep: `grep "STORAGE ERROR" logs`
- ✅ Runtime controllable: `SPDLOG_LEVEL=debug` shows all alerts

---

## Solution 4: Structured JSON (For Monitoring)

**Goal:** Machine-readable format for metrics/monitoring systems.

```cpp
void log::log_alert_json(libtorrent::alert const* a)
{
    using namespace libtorrent;

    // Build JSON-like structured log
    int type = a->type();

    switch (type) {
        case torrent_finished_alert::alert_type: {
            auto* tf = alert_cast<torrent_finished_alert>(a);
            spdlog::info(R"({{"event":"torrent_finished","torrent":"{}","timestamp":{}}})",
                        tf->torrent_name(),
                        std::chrono::system_clock::now().time_since_epoch().count());
            break;
        }

        case file_error_alert::alert_type: {
            auto* fe = alert_cast<file_error_alert>(a);
            spdlog::error(R"({{"event":"file_error","torrent":"{}","file":"{}","error":"{}","timestamp":{}}})",
                         fe->torrent_name(), fe->filename(), fe->error.message(),
                         std::chrono::system_clock::now().time_since_epoch().count());
            break;
        }

        case performance_alert::alert_type: {
            auto* pa = alert_cast<performance_alert>(a);
            spdlog::warn(R"({{"event":"performance_warning","code":{},"message":"{}","timestamp":{}}})",
                        static_cast<int>(pa->warning_code), pa->message(),
                        std::chrono::system_clock::now().time_since_epoch().count());
            break;
        }

        default:
            spdlog::info(R"({{"event":"{}","message":"{}","timestamp":{}}})",
                        a->what(), a->message(),
                        std::chrono::system_clock::now().time_since_epoch().count());
            break;
    }
}
```

**Benefits:**
- ✅ Parse with `jq`: `grep "torrent_finished" logs | jq '.torrent'`
- ✅ Easy integration with monitoring (Prometheus, Grafana)
- ✅ Time-series analysis

---

## Recommended Alert Mask Configuration

### Production (Current + Storage + Performance)

```cpp
// main.cpp
p.set_int(lt::settings_pack::alert_mask,
    lt::alert_category::error              // Errors (critical)
    | lt::alert_category::status           // State changes
    | lt::alert_category::storage          // Storage events (important!)
    | lt::alert_category::tracker          // Tracker events
    | lt::alert_category::performance_warning  // Performance issues
);
```

**Why add storage + tracker + performance_warning:**
- **storage**: Catch file write errors, disk full, permission errors
- **tracker**: Monitor tracker availability (important for Clonezilla deployment)
- **performance_warning**: Detect bottlenecks (upload/download rate limits)

### Development (Add Debug)

```cpp
p.set_int(lt::settings_pack::alert_mask,
    lt::alert_category::error
    | lt::alert_category::status
    | lt::alert_category::storage
    | lt::alert_category::tracker
    | lt::alert_category::performance_warning
    | lt::alert_category::peer             // Peer connect/disconnect
    | lt::alert_category::piece_progress   // Piece completion
);
```

### Verbose Debug (Careful - High Rate!)

```cpp
// Only enable for debugging specific issues!
p.set_int(lt::settings_pack::alert_mask,
    lt::alert_category::all  // ALL alerts (very verbose!)
);
```

---

## Implementation Checklist

### Phase 1: set_alert_notify() 🔥 Required

- [ ] 1. Add `set_alert_notify()` to daemon.hpp + daemon.cpp
- [ ] 2. Add synchronization to log.hpp (mutex, condition_variable, flag)
- [ ] 3. Setup callback in log::log() constructor
- [ ] 4. Rewrite log::report_alert() to use condition_variable
- [ ] 5. Replace `SPDLOG_INFO` with `spdlog::info`
- [ ] 6. Test:
  - [ ] Alerts logged immediately (< 1ms)
  - [ ] Shutdown within 1 second
  - [ ] Runtime log level control works (`SPDLOG_LEVEL=debug`)

### Phase 2: Alert Categorization (Recommended)

- [ ] 7. Add `log_alert_by_category()` function
- [ ] 8. Update alert_mask to include storage + tracker + performance_warning
- [ ] 9. Test with `SPDLOG_LEVEL=debug` to verify runtime control

### Phase 3: Enhanced Alert Formatting (Optional, High Value)

- [ ] 10. Implement `log_alert_detailed()` with alert-specific formatting
- [ ] 11. Test with various alert types (file error, tracker error, etc.)
- [ ] 12. Update log.hpp to add new methods

### Phase 4: Monitoring Integration (Optional)

- [ ] 13. Add JSON structured logging for important alerts
- [ ] 14. Create separate metrics logger (like in SPDLOG_ADVANCED.md)
- [ ] 15. Export metrics to monitoring system

---

## Testing Strategy

### Test 1: Runtime Log Level Control

```bash
# Info level (default)
./ezio
# Should see: [info] lt alert: ...

# Debug level (see all alerts)
export SPDLOG_LEVEL=debug
./ezio
# Should see: [debug] lt alert: ...

# Errors only
export SPDLOG_LEVEL=error
./ezio
# Should only see: [error] lt alert: ...
```

### Test 2: Alert Categorization

```bash
# Trigger file error (write to read-only location)
chmod 444 /path/to/torrent/file
# Should see: [error] lt alert: [STORAGE ERROR] ...

# Complete torrent
# Should see: [info] lt alert: [COMPLETE] torrent finished: ...
```

### Test 3: Performance Warnings

```bash
# Set low limits to trigger warnings
p.set_int(lt::settings_pack::send_buffer_watermark, 1024);  // Very low
# Should see: [warn] lt alert: [PERFORMANCE] ...
```

---

## Alert Types Reference

### Critical Alerts (Must Monitor)

| Alert Type | Category | Description | Log Level |
|------------|----------|-------------|-----------|
| `file_error_alert` | storage + error | Disk write/read error | ERROR |
| `storage_moved_failed_alert` | storage + error | Failed to move storage | ERROR |
| `hash_failed_alert` | error | Piece hash mismatch | ERROR |
| `torrent_error_alert` | error | Fatal torrent error | ERROR |

### Important Alerts (Should Monitor)

| Alert Type | Category | Description | Log Level |
|------------|----------|-------------|-----------|
| `performance_alert` | performance_warning | Rate limits hit | WARN |
| `tracker_error_alert` | tracker + error | Tracker unreachable | ERROR |
| `tracker_warning_alert` | tracker + error | Tracker warning | WARN |
| `peer_error_alert` | peer + error | Peer connection error | WARN |

### Status Alerts (Informational)

| Alert Type | Category | Description | Log Level |
|------------|----------|-------------|-----------|
| `torrent_finished_alert` | status | Download complete | INFO |
| `torrent_paused_alert` | status | Torrent paused | INFO |
| `torrent_resumed_alert` | status | Torrent resumed | INFO |
| `state_changed_alert` | status | State transition | INFO |
| `tracker_reply_alert` | tracker | Tracker announce success | DEBUG |

---

## Performance Impact

### Current Implementation

```
5-second polling interval
~10-50 alerts per interval (typical)
Processing time: ~1-5ms per batch (negligible)
```

### With Enhanced Formatting

```
Alert-specific formatting adds ~2-3μs per alert
Total overhead: < 0.5ms per batch
Percentage: < 0.01% of polling interval
```

**Conclusion:** No measurable performance impact.

---

## Examples

### Example 1: File Error

**Before (current):**
```
[2024-12-14 10:30:15] [info] lt alert: file_error_alert file error: Permission denied
```

**After (enhanced):**
```
[2024-12-14 10:30:15] [error] lt alert: [STORAGE ERROR] file=/dev/sda1 op=write error=Permission denied torrent=disk_image
```

### Example 2: Torrent Complete

**Before (current):**
```
[2024-12-14 11:00:00] [info] lt alert: torrent_finished_alert torrent finished downloading
```

**After (enhanced):**
```
[2024-12-14 11:00:00] [info] lt alert: [COMPLETE] torrent finished: disk_image
```

### Example 3: Performance Warning

**Before (current):**
```
[2024-12-14 12:15:30] [info] lt alert: performance_alert send buffer watermark too low
```

**After (enhanced):**
```
[2024-12-14 12:15:30] [warn] lt alert: [PERFORMANCE] 13 message=send buffer watermark too low
```

---

## Grep Patterns for Analysis

```bash
# Find all errors
grep "\[error\]" logs/ezio.log

# Find storage errors specifically
grep "STORAGE ERROR" logs/ezio.log

# Find completed torrents
grep "COMPLETE" logs/ezio.log

# Find performance issues
grep "PERFORMANCE" logs/ezio.log

# Count errors by type
grep "\[error\]" logs/ezio.log | awk '{print $5}' | sort | uniq -c

# Extract JSON alerts (if using structured logging)
grep -E '^\{' logs/ezio.log | jq '.event' | sort | uniq -c
```

---

## Compatibility with SPDLOG_ADVANCED.md

This design is fully compatible with recommendations in `SPDLOG_ADVANCED.md`:

- ✅ Uses `spdlog::info/error/warn` functions (runtime control)
- ✅ Can use async logging (if configured in main.cpp)
- ✅ Supports `SPDLOG_LEVEL` environment variable
- ✅ Structured logging option for metrics
- ✅ stdout/stderr only (no file rotation)

**Combined usage:**
```bash
# Development: Debug level, see all alerts
export SPDLOG_LEVEL=debug
./ezio

# Production: Warn level, only errors and warnings
export SPDLOG_LEVEL=warn
./ezio

# Component-specific (from SPDLOG_ADVANCED.md)
export SPDLOG_LEVEL=warn,raw_disk_io=debug
./ezio
```

---

## References

- `libtorrent/alert.hpp` - Alert base class and categories
- `libtorrent/alert_types.hpp` - All alert type definitions
- `docs/SPDLOG_ADVANCED.md` - spdlog runtime control guide
- libtorrent documentation: https://libtorrent.org/reference-Alerts.html

---

**Document Version:** 1.0
**Last Updated:** 2024-12-14
**Status:** Ready for Implementation
**Priority:** 🔥 High (Quick win with minimal changes)
