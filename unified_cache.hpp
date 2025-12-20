#ifndef __UNIFIED_CACHE_HPP__
#define __UNIFIED_CACHE_HPP__

#include <array>
#include <list>
#include <mutex>
#include <unordered_map>

#include "store_buffer.hpp"	 // For torrent_location

namespace ezio
{

// Cache entry: 16KB fixed size block
struct cache_entry {
	torrent_location loc;  // (storage, piece, offset)
	char *buffer;  // 16KB, malloc'ed by cache
	bool dirty;	 // Needs writeback to disk?
	bool flushing;	// Currently being flushed to disk?

	// LRU metadata
	std::list<torrent_location>::iterator lru_iter;

	// Default constructor with zero-initialized location
	cache_entry() :
		loc(libtorrent::storage_index_t(0), libtorrent::piece_index_t(0), 0),
		buffer(nullptr),
		dirty(false),
		flushing(false)
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
		loc(other.loc), buffer(other.buffer), dirty(other.dirty), flushing(other.flushing), lru_iter(other.lru_iter)
	{
		other.buffer = nullptr;	 // Transfer ownership
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
			dirty = other.dirty;
			flushing = other.flushing;
			lru_iter = other.lru_iter;
			other.buffer = nullptr;	 // Transfer ownership
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

public:
	cache_partition() : m_max_entries(0)
	{
	}
	explicit cache_partition(size_t max_entries);

	// Insert or update cache entry
	// If dirty=true, marks as needs writeback
	bool insert(torrent_location const &loc, char const *data, bool dirty);

	// Try to get from cache
	// Calls function f with buffer pointer if found
	// Similar to store_buffer::get()
	template<typename Fun>
	bool get(torrent_location const &loc, Fun f)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		auto it = m_entries.find(loc);
		if (it != m_entries.end()) {
			// Move to front of LRU (most recently used)
			m_lru_list.erase(it->second.lru_iter);
			m_lru_list.push_front(loc);
			it->second.lru_iter = m_lru_list.begin();

			// Call function with buffer pointer
			f(it->second.buffer);
			return true;
		}
		return false;
	}

	// Get two entries at once
	// Similar to store_buffer::get2()
	template<typename Fun>
	int get2(torrent_location const &loc1, torrent_location const &loc2, Fun f)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		auto it1 = m_entries.find(loc1);
		auto it2 = m_entries.find(loc2);

		char const *buf1 = (it1 == m_entries.end()) ? nullptr : it1->second.buffer;
		char const *buf2 = (it2 == m_entries.end()) ? nullptr : it2->second.buffer;

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

	// Set flushing flag (before flush operation)
	void set_flushing(torrent_location const &loc, bool value);

	// Mark clean if still flushing and not dirty (atomic check)
	// Returns true if marked clean, false if entry was modified during flush
	bool mark_clean_if_flushing(torrent_location const &loc);

	// Collect all dirty blocks in this partition and mark them as flushing
	std::vector<torrent_location> collect_dirty_blocks();

	// Statistics
	size_t size() const;
	size_t dirty_count() const;
	size_t max_entries() const
	{
		return m_max_entries;
	}

	// Dynamic resize
	void set_max_entries(size_t new_max);

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
	bool insert_write(torrent_location const &loc, char const *data);

	// Read operation: insert clean entry (from disk read)
	bool insert_read(torrent_location const &loc, char const *data);

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

	// Set flushing flag for a block
	void set_flushing(torrent_location const &loc, bool value);

	// Mark clean if still flushing and not dirty (atomic check)
	bool mark_clean_if_flushing(torrent_location const &loc);

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

private:
	size_t get_partition_index(torrent_location const &loc) const
	{
		return static_cast<size_t>(loc.piece) % NUM_PARTITIONS;
	}
};

}  // namespace ezio

#endif	// __UNIFIED_CACHE_HPP__
