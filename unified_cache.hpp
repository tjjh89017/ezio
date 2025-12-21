#ifndef __UNIFIED_CACHE_HPP__
#define __UNIFIED_CACHE_HPP__

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/asio/thread_pool.hpp>
#include <libtorrent/libtorrent.hpp>
#include <spdlog/spdlog.h>

#include "store_buffer.hpp"	 // For torrent_location

namespace ezio
{

// Single LRU design: all blocks in one list, use dirty flag only

// Per-partition statistics snapshot (non-atomic for copying)
struct cache_partition_stats {
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t inserts = 0;
	uint64_t evictions = 0;
};

// Per-LRU-list statistics snapshot
struct lru_stats_snapshot {
	uint64_t size = 0;	// current number of entries
	uint64_t inserts = 0;  // blocks inserted into this list
	uint64_t hits = 0;	// cache hits in this list
	uint64_t promotions = 0;  // moves to higher priority list
	uint64_t evictions = 0;	 // blocks evicted from this list
};

// Internal atomic statistics (not copyable)
struct cache_partition_stats_internal {
	std::atomic<uint64_t> hits{0};
	std::atomic<uint64_t> misses{0};
	std::atomic<uint64_t> inserts{0};
	std::atomic<uint64_t> evictions{0};

	void reset()
	{
		hits = 0;
		misses = 0;
		inserts = 0;
		evictions = 0;
	}

	// Convert to snapshot
	cache_partition_stats snapshot() const
	{
		cache_partition_stats s;
		s.hits = hits.load();
		s.misses = misses.load();
		s.inserts = inserts.load();
		s.evictions = evictions.load();
		return s;
	}
};

// Per-LRU-list atomic statistics
struct lru_stats_internal {
	std::atomic<uint64_t> inserts{0};
	std::atomic<uint64_t> hits{0};
	std::atomic<uint64_t> promotions{0};
	std::atomic<uint64_t> evictions{0};

	void reset()
	{
		inserts = 0;
		hits = 0;
		promotions = 0;
		evictions = 0;
	}

	lru_stats_snapshot snapshot(size_t current_size) const
	{
		lru_stats_snapshot s;
		s.size = current_size;
		s.inserts = inserts.load();
		s.hits = hits.load();
		s.promotions = promotions.load();
		s.evictions = evictions.load();
		return s;
	}
};

// Cache entry: 16KB fixed size block (buffer always allocated as 16KB, but actual data size may vary)
struct cache_entry {
	torrent_location loc;  // (storage, piece, offset)
	char *buffer;  // 16KB buffer, malloc'ed by cache
	int length;	 // Actual data size in buffer (can be < 16KB for last block)
	bool dirty;	 // Needs writeback to disk?

	// LRU metadata: iterator pointing to this entry's position in the single LRU list
	std::list<torrent_location>::iterator lru_iter;

	// Default constructor with zero-initialized location
	cache_entry() :
		loc(libtorrent::storage_index_t(0), libtorrent::piece_index_t(0), 0),
		buffer(nullptr),
		length(0),
		dirty(false)
	{
	}

	~cache_entry()
	{
		if (buffer) {
			free(buffer);
			buffer = nullptr;
		}
	}

	// Move constructor
	cache_entry(cache_entry &&other) noexcept :
		loc(other.loc),
		buffer(other.buffer),
		length(other.length),
		dirty(other.dirty),
		lru_iter(other.lru_iter)
	{
		other.buffer = nullptr;	 // Transfer ownership
		other.length = 0;
	}

	// Move assignment
	cache_entry &operator=(cache_entry &&other) noexcept
	{
		if (this != &other) {
			if (buffer) {
				free(buffer);
			}
			loc = other.loc;
			buffer = other.buffer;
			length = other.length;
			dirty = other.dirty;
			lru_iter = other.lru_iter;
			other.buffer = nullptr;	 // Transfer ownership
			other.length = 0;
		}
		return *this;
	}

	// Delete copy constructor and copy assignment
	cache_entry(cache_entry const &) = delete;
	cache_entry &operator=(cache_entry const &) = delete;
};

// Single cache partition (lock-free with 1:1 thread:partition mapping)
class cache_partition
{
private:
	std::unordered_map<torrent_location, cache_entry> m_entries;

	// Single LRU list (all blocks, both dirty and clean)
	std::list<torrent_location> m_lru;	// unified LRU list

	size_t m_max_entries;
	boost::asio::thread_pool m_callback_pool;  // Fixed 2 threads for watermark callbacks
	cache_partition_stats_internal m_stats;	 // Performance statistics (atomic)

	// Watermark tracking (for backpressure)
	size_t m_num_dirty{0};	// count of dirty blocks
	std::atomic<bool> m_exceeded{false};  // true = cache under pressure

	// Observer list for watermark recovery notification
	std::vector<std::weak_ptr<libtorrent::disk_observer>> m_observers;

	// Watermark thresholds (configurable)
	static constexpr float DIRTY_HIGH_WATERMARK = 0.875f;  // 87.5% dirty = stop writes
	static constexpr float DIRTY_LOW_WATERMARK = 0.50f;	 // 50% dirty = resume writes

public:
	cache_partition() : m_max_entries(0), m_callback_pool(1)
	{
	}
	cache_partition(size_t max_entries) :
		m_max_entries(max_entries), m_callback_pool(1)
	{
	}

	// Insert or update cache entry
	// If dirty=true, marks as needs writeback
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	// Returns true on success, false if failed to allocate buffer
	// Note: Even if watermark is exceeded, insert will succeed (allows over-allocation)
	// Caller should check watermark separately via check_watermark()
	bool insert(torrent_location const &loc, char const *data, int length, bool dirty);

	// Try to get from cache
	// Calls function f with buffer pointer if found
	// Similar to store_buffer::get()
	// Get with exclusive access (for I/O thread owning this partition)
	// Can touch (modify LRU)
	// Lock-free: 1:1 thread:partition mapping ensures single-threaded access
	template<typename Fun>
	bool get(torrent_location const &loc, Fun f)
	{
		auto it = m_entries.find(loc);
		if (it != m_entries.end()) {
			// Cache hit - touch (move to front of LRU)
			m_stats.hits++;

			// Single LRU touch: move to front (most recently used)
			m_lru.erase(it->second.lru_iter);
			m_lru.push_front(loc);
			it->second.lru_iter = m_lru.begin();

			// Call function with buffer pointer
			f(it->second.buffer);
			return true;
		}

		// Cache miss
		m_stats.misses++;
		return false;
	}

	// Get two entries at once
	// Similar to store_buffer::get2()
	// Lock-free: 1:1 thread:partition mapping ensures single-threaded access
	template<typename Fun>
	int get2(torrent_location const &loc1, torrent_location const &loc2, Fun f)
	{
		auto it1 = m_entries.find(loc1);
		auto it2 = m_entries.find(loc2);

		char const *buf1 = (it1 == m_entries.end()) ? nullptr : it1->second.buffer;
		char const *buf2 = (it2 == m_entries.end()) ? nullptr : it2->second.buffer;

		// Update hit/miss stats
		if (buf1)
			m_stats.hits++;
		else
			m_stats.misses++;
		if (buf2)
			m_stats.hits++;
		else
			m_stats.misses++;

		if (buf1 == nullptr && buf2 == nullptr) {
			return 0;
		}

		// Single LRU: touch found entries (move to front)
		if (buf1) {
			m_lru.erase(it1->second.lru_iter);
			m_lru.push_front(loc1);
			it1->second.lru_iter = m_lru.begin();
		}

		if (buf2) {
			m_lru.erase(it2->second.lru_iter);
			m_lru.push_front(loc2);
			it2->second.lru_iter = m_lru.begin();
		}

		return f(buf1, buf2);
	}

	// Mark entry as clean (writeback completed)
	// Entry remains in cache for future reads
	void mark_clean(torrent_location const &loc);

	// Get length of entry (returns 0 if not found)
	int get_length(torrent_location const &loc) const
	{
		auto it = m_entries.find(loc);
		return (it == m_entries.end()) ? 0 : it->second.length;
	}

	// Collect all dirty blocks in this partition and mark them as clean
	std::vector<torrent_location> collect_dirty_blocks();

	// Collect dirty blocks for a specific storage only
	std::vector<torrent_location> collect_dirty_blocks_for_storage(libtorrent::storage_index_t storage);

	// Statistics
	size_t size() const;
	size_t dirty_count() const;
	size_t max_entries() const
	{
		return m_max_entries;
	}

	// Dynamic resize
	void set_max_entries(size_t new_max);

	// Get statistics snapshot (for diagnostics)
	cache_partition_stats get_stats() const
	{
		return m_stats.snapshot();
	}

	void reset_stats()
	{
		m_stats.reset();
		// Single LRU - no per-list stats
	}

	// Get per-LRU-list statistics snapshots
	// Single LRU - no per-list stats needed

	// Watermark checking (disabled for performance testing)
	// Returns true if cache is OK, false if exceeded (caller should pause writes)
	// If exceeded, saves observer for later notification
	bool check_watermark(std::shared_ptr<libtorrent::disk_observer> o)
	{
		// Watermark mechanism disabled - always return true (OK to insert)
		return true;
		/*
		if (m_max_entries == 0)
			return true;

		float dirty_ratio = static_cast<float>(m_num_dirty) / m_max_entries;

		if (!m_exceeded && dirty_ratio > DIRTY_HIGH_WATERMARK) {
			// Exceeded high watermark - cache under pressure
			m_exceeded = true;
			spdlog::warn("[cache_partition] Watermark EXCEEDED ON: dirty={}/{} ({:.1f}%) > {:.1f}%",
				m_num_dirty, m_max_entries, dirty_ratio * 100.0, DIRTY_HIGH_WATERMARK * 100.0);
			if (o)
				m_observers.push_back(o);
			return false;
		}

		if (m_exceeded && dirty_ratio < DIRTY_LOW_WATERMARK) {
			// Recovered below low watermark
			m_exceeded = false;
			spdlog::info("[cache_partition] Watermark EXCEEDED OFF: dirty={}/{} ({:.1f}%) < {:.1f}%",
				m_num_dirty, m_max_entries, dirty_ratio * 100.0, DIRTY_LOW_WATERMARK * 100.0);
			return true;
		}

		// Still exceeded - save observer
		if (m_exceeded && o) {
			m_observers.push_back(o);
		}

		return !m_exceeded;	 // Current state
		*/
	}

	// Get current watermark status
	bool is_exceeded() const
	{
		return m_exceeded;
	}

	// Get dirty ratio (0.0 to 1.0)
	float get_dirty_ratio() const
	{
		if (m_max_entries == 0)
			return 0.0f;
		return static_cast<float>(m_num_dirty) / m_max_entries;
	}

	// Check watermark recovery (called after mark_clean or eviction)
	// Similar to buffer_pool::check_buffer_level
	void check_buffer_level();

private:
	// LRU eviction (O(1) with multi-LRU design)
	// Returns false if cannot evict (e.g., all entries are dirty)
	bool evict_one_lru();

	// Move entry to front of LRU (most recently used)
	void touch(torrent_location const &loc);

	// Move entry to a different LRU list (for state transitions)
};

// Unified cache with dynamic partitions (sharded for concurrency)
class unified_cache
{
private:
	std::vector<std::unique_ptr<cache_partition>> m_partitions;	 // Dynamic size (= num_io_threads)
	size_t m_max_entries;  // Total capacity across all partitions

public:
	// Constructor: max_entries = total cache size / 16KB
	// Example: 512MB = (512 * 1024 * 1024) / 16384 = 32768 entries
	explicit unified_cache(size_t max_entries);

	// Resize partitions (called during initialization)
	// num_partitions: number of I/O threads (= number of partitions)
	// entries_per_partition: cache entries per partition
	void resize_partitions(size_t num_partitions, size_t entries_per_partition);

	// Write operation: insert and mark dirty
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	// exceeded: output parameter, set to true if watermark exceeded
	// o: disk_observer to notify when cache recovers (optional)
	// Returns true on success, false if failed to allocate buffer
	bool insert_write(torrent_location const &loc, char const *data, int length, bool &exceeded,
		std::shared_ptr<libtorrent::disk_observer> o = nullptr);

	// Read operation: insert clean entry (from disk read)
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	bool insert_read(torrent_location const &loc, char const *data, int length);

	// Try to get from cache
	// Calls function f with buffer pointer if found
	template<typename Fun>
	bool get(torrent_location const &loc, Fun f)
	{
		size_t partition_idx = get_partition_index(loc);
		return m_partitions[partition_idx]->get(loc, f);
	}

	// Get two entries at once
	template<typename Fun>
	int get2(torrent_location const &loc1, torrent_location const &loc2, Fun f)
	{
		size_t partition_idx1 = get_partition_index(loc1);
		size_t partition_idx2 = get_partition_index(loc2);

		// If same partition, use single-partition get2
		if (partition_idx1 == partition_idx2) {
			return m_partitions[partition_idx1]->get2(loc1, loc2, f);
		}

		// Different partitions - need to handle separately
		// Get from partition 1
		char const *buf1 = nullptr;
		bool found1 = m_partitions[partition_idx1]->get(loc1, [&](char const *b) {
			buf1 = b;
		});

		// Get from partition 2
		char const *buf2 = nullptr;
		bool found2 = m_partitions[partition_idx2]->get(loc2, [&](char const *b) {
			buf2 = b;
		});

		if (!found1 && !found2) {
			return 0;
		}

		return f(buf1, buf2);
	}

	// Mark as clean after writeback completes
	void mark_clean(torrent_location const &loc);

	// Get length of entry (returns 0 if not found)
	int get_length(torrent_location const &loc) const
	{
		size_t partition_idx = get_partition_index(loc);
		return m_partitions[partition_idx]->get_length(loc);
	}

	// Collect all dirty blocks for a storage
	std::vector<torrent_location> collect_dirty_blocks(libtorrent::storage_index_t storage);

	// Statistics
	size_t total_entries() const;
	size_t total_dirty_count() const;
	size_t get_dirty_count(libtorrent::storage_index_t storage);
	size_t max_entries() const
	{
		return m_max_entries;
	}
	size_t total_size_mb() const
	{
		return (total_entries() * 16) / 1024;
	}

	// Dynamic resize (from settings_updated)
	void set_max_entries(size_t new_max);

	// Usage percentage (for flush triggers), returns 0-100
	int usage_percentage() const
	{
		if (m_max_entries == 0)
			return 0;
		return static_cast<int>((total_entries() * 100) / m_max_entries);
	}

	// Get per-partition statistics
	std::vector<cache_partition_stats> get_partition_stats() const;

	// Get aggregated statistics (all partitions combined)
	cache_partition_stats get_aggregated_stats() const;

	// Reset all statistics
	void reset_stats();

	// Log detailed statistics (for debugging)
	void log_stats() const;

	// Check watermark for a specific location (with observer)
	// Returns true if OK to insert, false if exceeded (caller should pause writes)
	// Note: This modifies m_exceeded state in the partition, so it's not const
	bool check_watermark(torrent_location const &loc, std::shared_ptr<libtorrent::disk_observer> o)
	{
		size_t partition_idx = get_partition_index(loc);
		return m_partitions[partition_idx]->check_watermark(o);
	}

	// Check watermark for a specific location (without observer)
	// Simpler version for read-only checks
	bool check_watermark_readonly(torrent_location const &loc) const
	{
		size_t partition_idx = get_partition_index(loc);
		return !m_partitions[partition_idx]->is_exceeded();
	}

	// Check if any partition is exceeded
	bool is_any_partition_exceeded() const
	{
		for (size_t i = 0; i < m_partitions.size(); ++i) {
			if (m_partitions[i]->is_exceeded()) {
				return true;
			}
		}
		return false;
	}

private:
	size_t get_partition_index(torrent_location const &loc) const
	{
		// Hash only storage + piece (NOT offset)
		// This ensures all blocks of same piece go to same partition/thread
		// Important for: hash operations can access all blocks without cross-partition
		size_t h = 0;
		h ^= std::hash<int>{}(static_cast<int>(loc.torrent));
		h ^= std::hash<int>{}(static_cast<int>(loc.piece));
		return h % m_partitions.size();
	}
};

}  // namespace ezio

#endif	// __UNIFIED_CACHE_HPP__
