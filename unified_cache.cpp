#include "unified_cache.hpp"

#include <cstring>	// for memcpy
#include <spdlog/spdlog.h>

#include "buffer_pool.hpp"	// for DEFAULT_BLOCK_SIZE

namespace ezio
{

// NOTE: C++17 inline variables would eliminate the need for this definition
constexpr size_t unified_cache::NUM_PARTITIONS;

// ============================================================================
// cache_partition implementation
// ============================================================================

cache_partition::cache_partition(size_t max_entries) : m_max_entries(max_entries)
{
}

bool cache_partition::insert(torrent_location const &loc, char const *data, int length, bool dirty)
{
	std::unique_lock<std::mutex> l(m_mutex);

	// Check if entry already exists (update case)
	auto it = m_entries.find(loc);
	if (it != m_entries.end()) {
		// Entry exists - update it
		memcpy(it->second.buffer, data, length);
		it->second.length = length;
		it->second.dirty = dirty;

		// Move to front of LRU (most recently used)
		m_lru_list.erase(it->second.lru_iter);
		m_lru_list.push_front(loc);
		it->second.lru_iter = m_lru_list.begin();

		return true;
	}

	// New entry - check capacity and evict if needed
	while (m_entries.size() >= m_max_entries) {
		if (!evict_one_lru()) {
			spdlog::warn("[cache_partition] Cannot evict: all entries are dirty or cache is empty");
			return false;  // Cannot evict, cache is full
		}
	}

	// Allocate new buffer (cache manages its own memory)
	char *buffer = static_cast<char *>(malloc(DEFAULT_BLOCK_SIZE));
	if (!buffer) {
		spdlog::error("[cache_partition] malloc failed for 16KB buffer");
		return false;
	}

	// Copy data to cache buffer
	memcpy(buffer, data, length);

	// Create new entry
	cache_entry entry;
	entry.loc = loc;
	entry.buffer = buffer;
	entry.length = length;
	entry.dirty = dirty;

	// Add to LRU list (most recently used = front)
	m_lru_list.push_front(loc);
	entry.lru_iter = m_lru_list.begin();

	// Insert into map
	m_entries.emplace(loc, std::move(entry));

	return true;
}

void cache_partition::mark_clean(torrent_location const &loc)
{
	std::unique_lock<std::mutex> l(m_mutex);

	auto it = m_entries.find(loc);
	if (it != m_entries.end()) {
		it->second.dirty = false;

		// Note: Entry remains in cache for future reads!
		// This is the key difference from store_buffer
	}
}

std::vector<torrent_location> cache_partition::collect_dirty_blocks()
{
	std::unique_lock<std::mutex> l(m_mutex);

	std::vector<torrent_location> dirty_blocks;
	dirty_blocks.reserve(m_entries.size());

	// NOTE: C++17 could use structured bindings: for (auto &[loc, entry] : m_entries)
	for (auto &pair : m_entries) {
		// Collect dirty blocks
		if (pair.second.dirty) {
			dirty_blocks.push_back(pair.first);
			// Clear dirty flag (will be written to disk)
			pair.second.dirty = false;
		}
	}

	return dirty_blocks;
}

std::vector<torrent_location> cache_partition::collect_dirty_blocks_for_storage(
	libtorrent::storage_index_t storage)
{
	std::unique_lock<std::mutex> l(m_mutex);

	std::vector<torrent_location> dirty_blocks;
	dirty_blocks.reserve(m_entries.size());

	// Only collect dirty blocks for the specified storage
	for (auto &pair : m_entries) {
		if (pair.second.dirty && pair.first.torrent == storage) {
			dirty_blocks.push_back(pair.first);
			pair.second.dirty = false;
		}
	}

	return dirty_blocks;
}

size_t cache_partition::size() const
{
	std::unique_lock<std::mutex> l(m_mutex);
	return m_entries.size();
}

size_t cache_partition::dirty_count() const
{
	std::unique_lock<std::mutex> l(m_mutex);

	size_t count = 0;
	// NOTE: C++17 could use structured bindings: for (auto const &[loc, entry] : m_entries)
	for (auto const &pair : m_entries) {
		if (pair.second.dirty) {
			++count;
		}
	}
	return count;
}

void cache_partition::set_max_entries(size_t new_max)
{
	std::unique_lock<std::mutex> l(m_mutex);
	m_max_entries = new_max;

	// If shrinking, evict entries until size <= new_max
	while (m_entries.size() > m_max_entries) {
		if (!evict_one_lru()) {
			spdlog::warn("[cache_partition] Cannot shrink: too many dirty entries");
			break;
		}
	}
}

bool cache_partition::evict_one_lru()
{
	// NOTE: Caller must hold m_mutex!

	if (m_lru_list.empty()) {
		return false;  // Nothing to evict
	}

	// Scan up to 32 LRU entries to find a clean one
	// This avoids failing immediately if LRU entry is dirty
	constexpr size_t MAX_SCAN = 32;
	size_t scanned = 0;
	size_t dirty_count = 0;

	auto lru_it = m_lru_list.rbegin();	// Start from least recently used
	while (lru_it != m_lru_list.rend() && scanned < MAX_SCAN) {
		torrent_location const &loc = *lru_it;
		auto it = m_entries.find(loc);

		if (it == m_entries.end()) {
			// Inconsistency - this shouldn't happen
			spdlog::error("[cache_partition] LRU inconsistency: entry not found in map");
			++lru_it;
			++scanned;
			continue;
		}

		// Skip dirty entries (will be flushed later)
		if (it->second.dirty) {
			++dirty_count;
			++lru_it;
			++scanned;
			continue;
		}

		// Found a clean entry - evict it
		// Remove from map (cache_entry destructor will free the buffer)
		m_entries.erase(it);

		// Remove from LRU list (need to convert reverse_iterator to iterator)
		auto forward_it = std::next(lru_it).base();
		m_lru_list.erase(forward_it);

		return true;
	}

	// Scanned MAX_SCAN entries but all were dirty
	spdlog::warn("[cache_partition] Cannot evict: scanned {} entries, {} dirty",
		scanned, dirty_count);
	return false;
}

void cache_partition::touch(torrent_location const &loc)
{
	// NOTE: Caller must hold m_mutex!

	auto it = m_entries.find(loc);
	if (it != m_entries.end()) {
		// Move to front of LRU
		m_lru_list.erase(it->second.lru_iter);
		m_lru_list.push_front(loc);
		it->second.lru_iter = m_lru_list.begin();
	}
}

// ============================================================================
// unified_cache implementation
// ============================================================================

unified_cache::unified_cache(size_t max_entries) : m_max_entries(max_entries)
{
	// Distribute entries evenly across partitions
	size_t entries_per_partition = max_entries / NUM_PARTITIONS;

	// Note: Cannot assign cache_partition because mutex is not copyable/movable
	// Array is already default-constructed, just set max_entries for each
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		m_partitions[i].set_max_entries(entries_per_partition);
	}

	spdlog::info("[unified_cache] Initialized with {} entries ({} MB), {} partitions", max_entries,
		(max_entries * 16) / 1024, NUM_PARTITIONS);
}

bool unified_cache::insert_write(torrent_location const &loc, char const *data, int length)
{
	size_t partition_idx = get_partition_index(loc);
	return m_partitions[partition_idx].insert(loc, data, length, true);  // dirty=true
}

bool unified_cache::insert_read(torrent_location const &loc, char const *data, int length)
{
	size_t partition_idx = get_partition_index(loc);
	return m_partitions[partition_idx].insert(loc, data, length, false);  // dirty=false
}

void unified_cache::mark_clean(torrent_location const &loc)
{
	size_t partition_idx = get_partition_index(loc);
	m_partitions[partition_idx].mark_clean(loc);
}

std::vector<torrent_location> unified_cache::collect_dirty_blocks(libtorrent::storage_index_t storage)
{
	std::vector<torrent_location> all_dirty;

	// Collect from all partitions, but only for the specified storage
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		auto partition_dirty = m_partitions[i].collect_dirty_blocks_for_storage(storage);
		all_dirty.insert(all_dirty.end(), partition_dirty.begin(), partition_dirty.end());
	}

	return all_dirty;
}

size_t unified_cache::total_entries() const
{
	size_t total = 0;
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		total += m_partitions[i].size();
	}
	return total;
}

size_t unified_cache::total_dirty_count() const
{
	size_t total = 0;
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		total += m_partitions[i].dirty_count();
	}
	return total;
}

size_t unified_cache::get_dirty_count(libtorrent::storage_index_t storage)
{
	// Collect dirty blocks for this storage
	size_t count = 0;

	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		auto partition_dirty = m_partitions[i].collect_dirty_blocks();

		for (auto const &loc : partition_dirty) {
			if (loc.torrent == storage) {
				++count;
			}
		}
	}

	return count;
}

void unified_cache::set_max_entries(size_t new_max)
{
	m_max_entries = new_max;
	size_t entries_per_partition = new_max / NUM_PARTITIONS;

	// Resize each partition
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		m_partitions[i].set_max_entries(entries_per_partition);
	}

	spdlog::info("[unified_cache] Resized to {} entries ({} MB)", new_max, (new_max * 16) / 1024);
}

}  // namespace ezio
