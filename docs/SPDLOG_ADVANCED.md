# spdlog Advanced Usage Guide for EZIO

**Version:** 1.0
**Date:** 2024-12-14
**Status:** Implementation Guide

---

## Current State Analysis

### What EZIO Already Has ✅

```cpp
// main.cpp:7,19
#include "spdlog/cfg/env.h"
spdlog::cfg::load_env_levels();
```

**This already enables runtime log level control via environment variables!**

### Current Usage Pattern (Problem)

```cpp
// log.cpp - Uses macros
SPDLOG_INFO("start speed report thread");
SPDLOG_INFO("[{}][{}%][D: {:.2f}MB/s]", ...);
```

**Problem with Macros:**
- `SPDLOG_*` macros are **compile-time checked**
- Cannot be controlled at runtime effectively
- If compiled with `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO`, debug logs are stripped out entirely

---

## Solution: Use Runtime Logger Functions

### 1. Replace Macros with Logger Functions

**Before (Macro - Compile-time):**
```cpp
SPDLOG_TRACE("trace message");
SPDLOG_DEBUG("debug message");
SPDLOG_INFO("info message");
SPDLOG_WARN("warning message");
SPDLOG_ERROR("error message");
SPDLOG_CRITICAL("critical message");
```

**After (Function - Runtime):**
```cpp
spdlog::trace("trace message");
spdlog::debug("debug message");
spdlog::info("info message");
spdlog::warn("warning message");
spdlog::error("error message");
spdlog::critical("critical message");
```

**Benefits:**
- ✅ Full runtime control via environment variables
- ✅ Can change level without recompiling
- ✅ Still zero-overhead when level is disabled (inline functions)
- ✅ Format strings are still compile-time checked (fmt library)

**Performance:**
- **Macros:** Level check at compile time → faster compile, less flexible
- **Functions:** Level check at runtime → slightly slower (~1-2 ns), but negligible

**Recommendation:** Use functions for flexibility. The overhead is unmeasurable in I/O-bound application.

---

## 2. Environment Variable Control

### Basic Usage (Already Working)

```bash
# Set log level for all loggers
export SPDLOG_LEVEL=debug
./ezio

# Set different levels for different components
export SPDLOG_LEVEL=info,ezio::buffer_pool=debug,ezio::raw_disk_io=trace
./ezio
```

### Available Log Levels

```
trace    (most verbose)
debug
info     (default)
warn
error
critical (least verbose)
off      (disable logging)
```

### Examples

```bash
# Production: Only warnings and errors
export SPDLOG_LEVEL=warn
./ezio

# Development: Everything except trace
export SPDLOG_LEVEL=debug
./ezio

# Debugging specific component
export SPDLOG_LEVEL=info,raw_disk_io=trace
./ezio

# Performance profiling: Disable all logging
export SPDLOG_LEVEL=off
./ezio
```

---

## 3. Structured Logging (Better Than Current)

### Current Style (Basic)
```cpp
SPDLOG_INFO("[{}][{}%][D: {:.2f}MB/s][U: {:.2f}MB/s]",
    t_stat.save_path,
    int(t_stat.progress * 100),
    (double)t_stat.download_rate / 1024 / 1024,
    (double)t_stat.upload_rate / 1024 / 1024);
```

**Problems:**
- Hard to parse
- Not machine-readable
- Difficult to filter/search

### Improved: Structured Logging

```cpp
// Use JSON-like format for important metrics
spdlog::info(R"({{"event":"transfer_status","path":"{}","progress":{:.2f},"download_mbps":{:.2f},"upload_mbps":{:.2f}}})",
    t_stat.save_path,
    t_stat.progress * 100,
    (double)t_stat.download_rate / 1024 / 1024,
    (double)t_stat.upload_rate / 1024 / 1024);

// Output:
// {"event":"transfer_status","path":"/dev/sda1","progress":45.32,"download_mbps":123.45,"upload_mbps":67.89}
```

**Benefits:**
- Easy to parse with `jq`, `grep`, etc.
- Can export to monitoring systems
- Machine-readable for analysis

**Or use a helper:**
```cpp
// Helper for structured logging
template<typename... Args>
void log_metric(const char* event, Args&&... args) {
    spdlog::info(R"({{"event":"{}",{}}})", event, fmt::format(std::forward<Args>(args)...));
}

// Usage
log_metric("transfer_status",
    R"("path":"{}","progress":{:.2f})",
    t_stat.save_path,
    t_stat.progress * 100);
```

---

## 4. Named Loggers (Component-Level Control)

### Current: Global Logger Only

```cpp
spdlog::info("message");  // Uses default logger
```

### Better: Named Loggers per Component

```cpp
// Create logger for each component
class raw_disk_io {
private:
    std::shared_ptr<spdlog::logger> m_logger;

public:
    raw_disk_io() {
        m_logger = spdlog::get("raw_disk_io");
        if (!m_logger) {
            m_logger = spdlog::default_logger()->clone("raw_disk_io");
            spdlog::register_logger(m_logger);
        }
    }

    void async_write(...) {
        m_logger->debug("async_write: piece={} offset={}", piece, offset);
        // ...
    }
};
```

**Then control individually:**
```bash
# Only debug raw_disk_io, rest at info
export SPDLOG_LEVEL=info,raw_disk_io=debug
./ezio
```

**Benefits:**
- Fine-grained control
- Can debug specific components without noise
- Each component can have different log level

---

## 5. Performance: Asynchronous Logging

### Current: Synchronous (Blocks on I/O)

```cpp
spdlog::info("message");  // Writes to stdout immediately
```

**Problem:** Each log call does I/O, slowing down hot path.

### Solution: Async Logger

```cpp
// main.cpp initialization
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

void setup_logging() {
    // Load env vars first
    spdlog::cfg::load_env_levels();

    // Create async logger (8KB queue, 1 background thread)
    auto async_file = spdlog::rotating_logger_mt<spdlog::async_factory>(
        "async_logger",
        "logs/ezio.log",
        1024 * 1024 * 100,  // 100 MB per file
        3                    // Keep 3 files
    );

    // Also log to console
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    async_file->sinks().push_back(console_sink);

    // Set as default
    spdlog::set_default_logger(async_file);

    // Set pattern
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
}

int main() {
    setup_logging();
    spdlog::info("EZIO started");
    // ...
}
```

**Benefits:**
- ✅ No I/O blocking in hot path
- ✅ Logs written by background thread
- ✅ 10-100x faster logging
- ✅ Rotating file support (prevents disk full)

**Trade-off:**
- Logs might be lost if process crashes before flush
- Small memory overhead (queue)

**Recommendation:** Use async for production, sync for debugging crashes.

---

## 6. Conditional Logging (Reduce Overhead)

### Problem: Expensive String Formatting

```cpp
// This formats string even if debug is disabled!
spdlog::debug("Expensive: {}", compute_expensive_string());
```

### Solution: Check Level First

```cpp
// Only format if debug is enabled
if (spdlog::should_log(spdlog::level::debug)) {
    spdlog::debug("Expensive: {}", compute_expensive_string());
}
```

**Or use lambda (cleaner):**
```cpp
// spdlog will only call lambda if level is enabled
spdlog::log(spdlog::level::debug, [&]() {
    return fmt::format("Expensive: {}", compute_expensive_string());
});
```

---

## 7. Context Tags (Better Filtering)

### Add Context to Logs

```cpp
// Tag logs with storage_index for filtering
class raw_disk_io {
    void async_write(storage_index_t storage, ...) {
        spdlog::info("[storage={}] async_write piece={}",
                    static_cast<int>(storage), piece);
    }
};

// Then grep easily:
// grep "storage=0" ezio.log
```

**Better: Thread-local context**
```cpp
// Set context for entire thread
spdlog::set_pattern("[%t][%@] %v");  // Add source location

// Or use RAII context:
struct log_context {
    log_context(const char* ctx) {
        // Push context
    }
    ~log_context() {
        // Pop context
    }
};

void process_torrent(torrent_info const& ti) {
    log_context ctx(ti.name());
    spdlog::info("Processing");  // Will include torrent name
}
```

---

## 8. Log Sampling (High-Frequency Events)

### Problem: Log Spam

```cpp
// Called thousands of times per second
void async_write(piece_index_t piece, ...) {
    spdlog::debug("async_write piece={}", piece);  // Too much!
}
```

### Solution: Sample Logs

```cpp
class sampled_logger {
private:
    std::atomic<uint64_t> m_call_count{0};
    uint64_t m_sample_rate;

public:
    sampled_logger(uint64_t rate = 1000) : m_sample_rate(rate) {}

    template<typename... Args>
    void log_sampled(spdlog::level::level_enum level,
                     spdlog::format_string_t<Args...> fmt,
                     Args&&... args) {
        uint64_t count = m_call_count.fetch_add(1, std::memory_order_relaxed);
        if (count % m_sample_rate == 0) {
            spdlog::log(level, fmt, std::forward<Args>(args)...);
        }
    }
};

// Usage
sampled_logger sampler(1000);  // Log 1 in 1000

void async_write(...) {
    sampler.log_sampled(spdlog::level::debug, "async_write piece={}", piece);
    // Only logs every 1000th call
}
```

---

## 9. Performance Metrics Logging

### Dedicated Performance Logger

```cpp
// Separate logger for metrics (always on, different format)
auto metrics_logger = spdlog::rotating_logger_mt(
    "metrics",
    "logs/metrics.log",
    1024 * 1024 * 10,  // 10 MB
    10                  // Keep 10 files
);

// CSV format for easy parsing
metrics_logger->set_pattern("%v");

// Log metrics
void log_metric(const char* name, double value) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    metrics_logger->info("{},{},{:.2f}", ms, name, value);
}

// Usage
log_metric("download_mbps", 123.45);
log_metric("buffer_pool_utilization", 78.3);

// Output (metrics.log):
// 1702531200000,download_mbps,123.45
// 1702531201000,buffer_pool_utilization,78.3
```

**Then analyze with:**
```bash
# Plot download rate over time
awk -F, '$2=="download_mbps" {print $1/1000, $3}' metrics.log | gnuplot ...
```

---

## 10. Recommended Configuration for EZIO

### main.cpp Enhancement (Simplified for stdout-only)

```cpp
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

void setup_logging() {
    // 1. Load environment variables
    spdlog::cfg::load_env_levels();

    // 2. Create async stdout logger
    spdlog::init_thread_pool(8192, 1);  // 8KB queue, 1 thread

    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    auto logger = std::make_shared<spdlog::async_logger>(
        "ezio",
        stdout_sink,
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::block
    );

    spdlog::set_default_logger(logger);

    // 3. Set pattern (simple, suitable for systemd/docker logs)
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    // 4. Apply env levels again (after logger created)
    spdlog::cfg::load_env_levels();

    spdlog::info("Logging initialized");
}

int main(int argc, char **argv) {
    setup_logging();  // Call first!

    // Rest of initialization...
    ezio::config current_config;
    current_config.parse_from_argv(argc, argv);

    spdlog::info("EZIO {} starting", GIT_VERSION);
    // ...
}
```

**Even Simpler (Keep Current Synchronous):**

If async complexity is not needed, just replace macros and keep current setup:

```cpp
int main(int argc, char **argv) {
    // Already has this:
    spdlog::cfg::load_env_levels();

    // Optionally set pattern
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    // That's it! Now use functions instead of macros
    spdlog::info("EZIO {} starting", GIT_VERSION);
    // ...
}
```

### Update All Files: Replace Macros

**Search & Replace:**
```bash
# Replace all macro calls with function calls
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_TRACE/spdlog::trace/g'
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_DEBUG/spdlog::debug/g'
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_INFO/spdlog::info/g'
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_WARN/spdlog::warn/g'
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_ERROR/spdlog::error/g'
find . -name "*.cpp" -o -name "*.hpp" | xargs sed -i 's/SPDLOG_CRITICAL/spdlog::critical/g'
```

---

## Usage Examples

### Development
```bash
# Debug everything
export SPDLOG_LEVEL=debug
./ezio

# Trace specific component
export SPDLOG_LEVEL=info,raw_disk_io=trace
./ezio
```

### Production
```bash
# Only important messages
export SPDLOG_LEVEL=warn
./ezio

# Or via systemd service:
[Service]
Environment="SPDLOG_LEVEL=warn"
ExecStart=/usr/local/bin/ezio
```

### Performance Testing
```bash
# Disable all logging
export SPDLOG_LEVEL=off
./ezio
```

### Log Analysis
```bash
# Find errors
grep "error" logs/ezio.log

# Monitor download rate
tail -f logs/ezio.log | grep "MB/s"

# Extract structured logs
grep "transfer_status" logs/ezio.log | jq '.download_mbps'
```

---

## Benefits Summary

| Feature | Current | Improved | Benefit |
|---------|---------|----------|---------|
| **Level Control** | Macro (compile-time) | Function (runtime) | ✅ Change without recompile |
| **Env Support** | ✅ Already has | ✅ Keep | ✅ Easy configuration |
| **Async Logging** | ❌ Synchronous | ✅ Async (optional) | ✅ 10-100x faster (if needed) |
| **Output** | ✅ stdout/stderr | ✅ Keep | ✅ Container-friendly |
| **Structured** | ❌ Hard to parse | ✅ JSON-like | ✅ Machine-readable |
| **Sampling** | ❌ No | ✅ Optional | ✅ Reduce spam |
| **Named Loggers** | ❌ Global only | ✅ Per-component | ✅ Fine-grained control |

---

## Implementation Checklist

### Minimal (Quick Win)
- [ ] 1. Replace `SPDLOG_*` macros with `spdlog::*` functions (search & replace)
- [ ] 2. Test with `export SPDLOG_LEVEL=debug`
- [ ] 3. Test with `export SPDLOG_LEVEL=trace,raw_disk_io=debug`

### Optional Enhancements
- [ ] 4. Add `setup_logging()` for async (if performance needed)
- [ ] 5. Add structured logging for important events
- [ ] 6. Add metrics logger for performance data
- [ ] 7. Update systemd service with environment variable
- [ ] 8. Document log levels in README

### Not Needed
- ~~Create `logs/` directory~~ (stdout only)
- ~~File rotation~~ (deployment clears logs)

---

## References

- [spdlog GitHub](https://github.com/gabime/spdlog)
- [spdlog Wiki](https://github.com/gabime/spdlog/wiki)
- [Environment Variables](https://github.com/gabime/spdlog/wiki/7.-Tweakme-and-custom-types#environment-variables)
- [Async Logging](https://github.com/gabime/spdlog/wiki/6.-Asynchronous-logging)

---

**Document Version:** 1.0
**Last Updated:** 2024-12-14
**Status:** Ready for Implementation
