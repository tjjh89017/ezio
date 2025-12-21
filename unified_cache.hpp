#ifndef __UNIFIED_CACHE_HPP__
#define __UNIFIED_CACHE_HPP__

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>

#include <libtorrent/libtorrent.hpp>

#include "store_buffer.hpp"	 // For torrent_location

namespace ezio
{

// Per-partition statistics snapshot (non-atomic for copying)
struct cache_partition_stats {
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t inserts = 0;
	uint64_t evictions = 0;
	uint64_t lock_contentions = 0;
	uint64_t total_lock_wait_us = 0;  // Microseconds spent waiting for lock
};

// Internal atomic statistics (not copyable)
struct cache_partition_stats_internal {
	std::atomic<uint64_t> hits{0};
	std::atomic<uint64_t> misses{0};
	std::atomic<uint64_t> inserts{0};
	std::atomic<uint64_t> evictions{0};
	std::atomic<uint64_t> lock_contentions{0};
	std::atomic<uint64_t> total_lock_wait_us{0};  // Microseconds spent waiting for lock

	void reset()
	{
		hits = 0;
		misses = 0;
		inserts = 0;
		evictions = 0;
		lock_contentions = 0;
		total_lock_wait_us = 0;
	}

	// Convert to snapshot
	cache_partition_stats snapshot() const
	{
		cache_partition_stats s;
		s.hits = hits.load();
		s.misses = misses.load();
		s.inserts = inserts.load();
		s.evictions = evictions.load();
		s.lock_contentions = lock_contentions.load();
		s.total_lock_wait_us = total_lock_wait_us.load();
		return s;
	}
};

// Cache entry: 16KB fixed size block (buffer always allocated as 16KB, but actual data size may vary)
struct cache_entry {
	torrent_location loc;  // (storage, piece, offset)
	char *buffer;  // 16KB buffer, malloc'ed by cache
	int length;	 // Actual data size in buffer (can be < 16KB for last block)
	bool dirty;	 // Needs writeback to disk?

	// LRU metadata
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

// Single cache partition with independent mutex
class cache_partition
{
private:
	mutable std::mutex m_mutex;	 // mutable for const methods
	std::unordered_map<torrent_location, cache_entry> m_entries;
	std::list<torrent_location> m_lru_list;	 // MRU at front, LRU at back
	size_t m_max_entries;
	cache_partition_stats_internal m_stats;	 // Performance statistics (atomic)

public:
	cache_partition() : m_max_entries(0)
	{
	}
	explicit cache_partition(size_t max_entries);

	// Insert or update cache entry
	// If dirty=true, marks as needs writeback
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	bool insert(torrent_location const &loc, char const *data, int length, bool dirty);

	// Try to get from cache
	// Calls function f with buffer pointer if found
	// Similar to store_buffer::get()
	template<typename Fun>
	bool get(torrent_location const &loc, Fun f)
	{
		// Measure lock wait time
		auto lock_start = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> l(m_mutex);
		auto lock_acquired = std::chrono::steady_clock::now();

		auto lock_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
			lock_acquired - lock_start)
								.count();

		if (lock_wait_us > 100) {  // More than 100us wait = contention
			m_stats.lock_contentions++;
		}
		m_stats.total_lock_wait_us += lock_wait_us;

		auto it = m_entries.find(loc);
		if (it != m_entries.end()) {
			// Cache hit
			m_stats.hits++;

			// Move to front of LRU (most recently used)
			m_lru_list.erase(it->second.lru_iter);
			m_lru_list.push_front(loc);
			it->second.lru_iter = m_lru_list.begin();

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
	template<typename Fun>
	int get2(torrent_location const &loc1, torrent_location const &loc2, Fun f)
	{
		// Measure lock wait time
		auto lock_start = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> l(m_mutex);
		auto lock_acquired = std::chrono::steady_clock::now();

		auto lock_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
			lock_acquired - lock_start)
								.count();

		if (lock_wait_us > 100) {  // More than 100us wait = contention
			m_stats.lock_contentions++;
		}
		m_stats.total_lock_wait_us += lock_wait_us;

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

		// Touch LRU for found entries
		if (buf1) {
			m_lru_list.erase(it1->second.lru_iter);
			m_lru_list.push_front(loc1);
			it1->second.lru_iter = m_lru_list.begin();
		}
		if (buf2) {
			m_lru_list.erase(it2->second.lru_iter);
			m_lru_list.push_front(loc2);
			it2->second.lru_iter = m_lru_list.begin();
		}

		return f(buf1, buf2);
	}

	// Mark entry as clean (writeback completed)
	// Entry remains in cache for future reads
	void mark_clean(torrent_location const &loc);

	// Get length of entry (returns 0 if not found)
	int get_length(torrent_location const &loc) const
	{
		std::unique_lock<std::mutex> l(m_mutex);
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
	}

private:
	// LRU eviction
	// Returns false if cannot evict (e.g., all entries are dirty)
	bool evict_one_lru();

	// Move entry to front of LRU (most recently used)
	void touch(torrent_location const &loc);
};

// Unified cache with 32 partitions (sharded for concurrency)
class unified_cache
{
private:
	static constexpr size_t NUM_PARTITIONS = 32;
	std::array<cache_partition, NUM_PARTITIONS> m_partitions;
	size_t m_max_entries;  // Total capacity across all partitions

public:
	// Constructor: max_entries = total cache size / 16KB
	// Example: 512MB = (512 * 1024 * 1024) / 16384 = 32768 entries
	explicit unified_cache(size_t max_entries);

	// Write operation: insert and mark dirty
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	bool insert_write(torrent_location const &loc, char const *data, int length);

	// Read operation: insert clean entry (from disk read)
	// length: actual data size (can be < DEFAULT_BLOCK_SIZE for last block)
	bool insert_read(torrent_location const &loc, char const *data, int length);

	// Try to get from cache
	// Calls function f with buffer pointer if found
	template<typename Fun>
	bool get(torrent_location const &loc, Fun f)
	{
		size_t partition_idx = get_partition_index(loc);
		return m_partitions[partition_idx].get(loc, f);
	}

	// Get two entries at once
	template<typename Fun>
	int get2(torrent_location const &loc1, torrent_location const &loc2, Fun f)
	{
		size_t partition_idx1 = get_partition_index(loc1);
		size_t partition_idx2 = get_partition_index(loc2);

		// If same partition, use single-partition get2
		if (partition_idx1 == partition_idx2) {
			return m_partitions[partition_idx1].get2(loc1, loc2, f);
		}

		// Different partitions - need to handle separately
		// Get from partition 1
		char const *buf1 = nullptr;
		bool found1 = m_partitions[partition_idx1].get(loc1, [&](char const *b) {
			buf1 = b;
		});

		// Get from partition 2
		char const *buf2 = nullptr;
		bool found2 = m_partitions[partition_idx2].get(loc2, [&](char const *b) {
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
		return m_partitions[partition_idx].get_length(loc);
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

private:
	size_t get_partition_index(torrent_location const &loc) const
	{
		// Full hash-based partitioning (better distribution across partitions)
		size_t hash = std::hash<torrent_location>{}(loc);
		return (hash >> 32) % NUM_PARTITIONS;

		// Alternative: Simple piece-based partitioning (lower overhead)
		// return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
	}
};

}  // namespace ezio

#endif	// __UNIFIED_CACHE_HPP__
