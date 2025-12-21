#include "unified_cache.hpp"

#include <chrono>  // for performance timing
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
	size_t evictions = 0;
	while (m_entries.size() >= m_max_entries) {
		if (!evict_one_lru()) {
			spdlog::warn("[cache_partition] Cannot evict: all entries are dirty or cache is empty");
			return false;  // Cannot evict, cache is full
		}
		evictions++;
	}
	m_stats.evictions += evictions;

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

	m_stats.inserts++;

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
	return m_partitions[partition_idx].insert(loc, data, length, true);	 // dirty=true
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

std::vector<cache_partition_stats> unified_cache::get_partition_stats() const
{
	std::vector<cache_partition_stats> stats;
	stats.reserve(NUM_PARTITIONS);

	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		stats.push_back(m_partitions[i].get_stats());
	}

	return stats;
}

cache_partition_stats unified_cache::get_aggregated_stats() const
{
	cache_partition_stats total;

	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		auto p_stats = m_partitions[i].get_stats();
		total.hits += p_stats.hits;
		total.misses += p_stats.misses;
		total.inserts += p_stats.inserts;
		total.evictions += p_stats.evictions;
		total.lock_contentions += p_stats.lock_contentions;
		total.total_lock_wait_us += p_stats.total_lock_wait_us;
	}

	return total;
}

void unified_cache::reset_stats()
{
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		m_partitions[i].reset_stats();
	}
}

void unified_cache::log_stats() const
{
	auto total = get_aggregated_stats();
	uint64_t total_ops = total.hits + total.misses;

	double hit_rate = (total_ops > 0) ? (100.0 * total.hits / total_ops) : 0.0;
	double avg_lock_wait_us = (total_ops > 0) ? (double(total.total_lock_wait_us) / total_ops) : 0.0;

	spdlog::info("[unified_cache] === Cache Performance Statistics ===");
	spdlog::info("[unified_cache] Total operations: {}", total_ops);
	spdlog::info("[unified_cache] Hits: {} ({:.2f}%)", total.hits, hit_rate);
	spdlog::info("[unified_cache] Misses: {} ({:.2f}%)", total.misses, 100.0 - hit_rate);
	spdlog::info("[unified_cache] Inserts: {}", total.inserts);
	spdlog::info("[unified_cache] Evictions: {}", total.evictions);
	spdlog::info("[unified_cache] Lock contentions (>100us): {}", total.lock_contentions);
	spdlog::info("[unified_cache] Avg lock wait: {:.2f} us", avg_lock_wait_us);
	spdlog::info("[unified_cache] Total entries: {} / {} ({:.1f}%)",
		total_entries(), m_max_entries, usage_percentage());
	spdlog::info("[unified_cache] Dirty entries: {}", total_dirty_count());

	// Per-partition distribution
	spdlog::info("[unified_cache] === Per-Partition Load Distribution ===");
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		size_t entries = m_partitions[i].size();
		size_t max_entries = m_partitions[i].max_entries();
		auto p_stats = m_partitions[i].get_stats();

		double usage = (max_entries > 0) ? (100.0 * entries / max_entries) : 0.0;
		uint64_t p_ops = p_stats.hits + p_stats.misses;
		double p_hit_rate = (p_ops > 0) ? (100.0 * p_stats.hits / p_ops) : 0.0;

		spdlog::info("[unified_cache]   Partition {:2d}: {:5d} entries ({:4.1f}%), "
					 "{:6d} ops, hit rate: {:5.2f}%, contentions: {:4d}",
			i, entries, usage, p_ops, p_hit_rate, p_stats.lock_contentions);
	}
}

}  // namespace ezio
