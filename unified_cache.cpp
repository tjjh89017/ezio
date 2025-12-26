#include "unified_cache.hpp"

#include <chrono>  // for performance timing
#include <cstring>	// for memcpy
#include <spdlog/spdlog.h>

#include "buffer_pool.hpp"	// for DEFAULT_BLOCK_SIZE

namespace ezio
{

// ============================================================================
// cache_partition implementation
// ============================================================================

bool cache_partition::insert(torrent_location const &loc, char const *data, int length, bool dirty)
{
	// Lock-free: 1:1 thread:partition mapping ensures single-threaded access
	// Check if entry already exists (update case)
	auto it = m_entries.find(loc);
	if (it != m_entries.end()) {
		// Entry exists - update it
		memcpy(it->second.buffer, data, length);
		it->second.length = length;

		// Update dirty flag and counters
		bool was_dirty = it->second.dirty;
		it->second.dirty = dirty;

		if (dirty && !was_dirty) {
			// Clean → dirty transition
			m_num_dirty++;
		} else if (!dirty && was_dirty) {
			// Dirty → clean transition
			m_num_dirty--;
		}

		// Single LRU: touch (move to front)
		m_lru.erase(it->second.lru_iter);
		m_lru.push_front(loc);
		it->second.lru_iter = m_lru.begin();

		return true;
	}

	// New entry - try to evict if at capacity
	// libtorrent 2.x design: allow over-allocation (short-term exceeding max_entries)
	// This prevents blocking writes when all entries are temporarily dirty
	bool did_evict = false;
	while (m_entries.size() >= m_max_entries) {
		if (!evict_one_lru()) {
			// Cannot evict (all clean entries exhausted)
			// Allow over-allocation - insert anyway
			spdlog::debug("[cache_partition] Over-allocation: size={}, max={}",
				m_entries.size() + 1, m_max_entries);
			break;
		}
		did_evict = true;
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

	// Single LRU: add to front (most recently used)
	m_lru.push_front(loc);
	entry.lru_iter = m_lru.begin();

	// Insert into map
	m_entries.emplace(loc, std::move(entry));

	m_stats.inserts++;

	// Update dirty counter
	if (dirty) {
		m_num_dirty++;
	}

	// Watermark mechanism disabled for performance testing
	// If cache is full and can't evict, insert() will return false
	// and caller will do sync_write
	// if (did_evict) {
	// 	check_buffer_level(l);
	// }

	return true;
}

void cache_partition::mark_clean(torrent_location const &loc)
{
	auto it = m_entries.find(loc);
	if (it != m_entries.end() && it->second.dirty) {
		it->second.dirty = false;

		// Update counters
		m_num_dirty--;

		spdlog::debug("[cache_partition] mark_clean: dirty {} -> {}",
			m_num_dirty + 1, m_num_dirty);

		// Watermark checking disabled
		// check_buffer_level(l);
	}
}

std::vector<torrent_location> cache_partition::collect_dirty_blocks()
{
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
	return m_entries.size();
}

size_t cache_partition::dirty_count() const
{
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
	// Single LRU eviction: scan from LRU end for first clean block
	// Skip dirty blocks (cannot evict while pending write)
	for (auto it = m_lru.rbegin(); it != m_lru.rend(); ++it) {
		auto entry_it = m_entries.find(*it);
		if (entry_it == m_entries.end()) {
			spdlog::error("[cache_partition] LRU inconsistency: entry not found");
			continue;
		}

		if (!entry_it->second.dirty) {
			// Found clean block - evict it
			m_entries.erase(entry_it);
			m_lru.erase(std::next(it).base());	// Convert reverse_iterator to iterator

			m_stats.evictions++;
			return true;
		}
	}

	// All entries are dirty - cannot evict
	// This is expected when cache is under write pressure
	return false;
}

// ============================================================================
// unified_cache implementation
// ============================================================================

unified_cache::unified_cache(size_t max_entries) :
	m_max_entries(max_entries)
{
	// Partitions will be initialized by resize_partitions()
	// Called from raw_disk_io constructor
}

void unified_cache::resize_partitions(size_t num_partitions, size_t entries_per_partition)
{
	// Resize vector and initialize each partition
	m_partitions.clear();
	m_partitions.reserve(num_partitions);

	for (size_t i = 0; i < num_partitions; ++i) {
		m_partitions.emplace_back(std::make_unique<cache_partition>(entries_per_partition));
	}

	spdlog::info("[unified_cache] Initialized {} partitions, {} entries per partition ({} KB)",
		num_partitions, entries_per_partition, (entries_per_partition * 16) / 1024);
	spdlog::info("[unified_cache] Total cache: {} entries ({} MB)",
		m_max_entries, (m_max_entries * 16) / 1024);
}

bool unified_cache::insert_write(torrent_location const &loc, char const *data, int length)
{
	size_t partition_idx = get_partition_index(loc);

	// Try to insert (allows over-allocation like libtorrent 2.x)
	// If insert fails (cache full), caller should handle it
	return m_partitions[partition_idx]->insert(loc, data, length, true);  // dirty=true
}

bool unified_cache::insert_read(torrent_location const &loc, char const *data, int length)
{
	size_t partition_idx = get_partition_index(loc);
	return m_partitions[partition_idx]->insert(loc, data, length, false);  // dirty=false
}

void unified_cache::mark_clean(torrent_location const &loc)
{
	size_t partition_idx = get_partition_index(loc);
	m_partitions[partition_idx]->mark_clean(loc);
}

std::vector<torrent_location> unified_cache::collect_dirty_blocks(libtorrent::storage_index_t storage)
{
	std::vector<torrent_location> all_dirty;

	// Collect from all partitions, but only for the specified storage
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		auto partition_dirty = m_partitions[i]->collect_dirty_blocks_for_storage(storage);
		all_dirty.insert(all_dirty.end(), partition_dirty.begin(), partition_dirty.end());
	}

	return all_dirty;
}

size_t unified_cache::total_entries() const
{
	size_t total = 0;
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		total += m_partitions[i]->size();
	}
	return total;
}

size_t unified_cache::total_dirty_count() const
{
	size_t total = 0;
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		total += m_partitions[i]->dirty_count();
	}
	return total;
}

size_t unified_cache::get_dirty_count(libtorrent::storage_index_t storage)
{
	// Collect dirty blocks for this storage
	size_t count = 0;

	for (size_t i = 0; i < m_partitions.size(); ++i) {
		auto partition_dirty = m_partitions[i]->collect_dirty_blocks();

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
	size_t entries_per_partition = new_max / m_partitions.size();

	// Resize each partition
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		m_partitions[i]->set_max_entries(entries_per_partition);
	}

	spdlog::info("[unified_cache] Resized to {} entries ({} MB)", new_max, (new_max * 16) / 1024);
}

std::vector<cache_partition_stats> unified_cache::get_partition_stats() const
{
	std::vector<cache_partition_stats> stats;
	stats.reserve(m_partitions.size());

	for (size_t i = 0; i < m_partitions.size(); ++i) {
		stats.push_back(m_partitions[i]->get_stats());
	}

	return stats;
}

cache_partition_stats unified_cache::get_aggregated_stats() const
{
	cache_partition_stats total;

	for (size_t i = 0; i < m_partitions.size(); ++i) {
		auto p_stats = m_partitions[i]->get_stats();
		total.hits += p_stats.hits;
		total.misses += p_stats.misses;
		total.inserts += p_stats.inserts;
		total.evictions += p_stats.evictions;
	}

	return total;
}

void unified_cache::reset_stats()
{
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		m_partitions[i]->reset_stats();
	}
}

void unified_cache::log_stats() const
{
	auto total = get_aggregated_stats();
	uint64_t total_ops = total.hits + total.misses;

	double hit_rate = (total_ops > 0) ? (100.0 * total.hits / total_ops) : 0.0;

	spdlog::info("[unified_cache] === Lock-Free Cache Performance Statistics ===");
	spdlog::info("[unified_cache] Total operations: {}", total_ops);
	spdlog::info("[unified_cache] Hits: {} ({:.2f}%)", total.hits, hit_rate);
	spdlog::info("[unified_cache] Misses: {} ({:.2f}%)", total.misses, 100.0 - hit_rate);
	spdlog::info("[unified_cache] Inserts: {}", total.inserts);
	spdlog::info("[unified_cache] Evictions: {}", total.evictions);
	spdlog::info("[unified_cache] Total entries: {} / {} ({:.1f}%)",
		total_entries(), m_max_entries, usage_percentage());
	spdlog::info("[unified_cache] Dirty entries: {}", total_dirty_count());

	// Single-LRU design: no per-list statistics needed

	// Watermark status (disabled)
	/*
	spdlog::info("[unified_cache] === Watermark Status ===");
	size_t exceeded_partitions = 0;
	float max_dirty_ratio = 0.0f;

	for (size_t i = 0; i < m_partitions.size(); ++i) {
		if (m_partitions[i]->is_exceeded()) {
			exceeded_partitions++;
		}
		float ratio = m_partitions[i]->get_dirty_ratio();
		if (ratio > max_dirty_ratio) {
			max_dirty_ratio = ratio;
		}
	}

	spdlog::info("[unified_cache]   Exceeded partitions: {} / {}", exceeded_partitions, m_partitions.size());
	spdlog::info("[unified_cache]   Max dirty ratio: {:.1f}%", max_dirty_ratio * 100.0);
	size_t total_ent = total_entries();
	spdlog::info("[unified_cache]   Global dirty ratio: {:.1f}%",
		(total_ent > 0) ? (100.0 * total_dirty_count() / total_ent) : 0.0);
	*/

	// Per-partition distribution (condensed)
	spdlog::info("[unified_cache] === Per-Partition Distribution ===");
	for (size_t i = 0; i < m_partitions.size(); ++i) {
		size_t entries = m_partitions[i]->size();
		size_t max_entries = m_partitions[i]->max_entries();
		auto p_stats = m_partitions[i]->get_stats();

		double usage = (max_entries > 0) ? (100.0 * entries / max_entries) : 0.0;
		uint64_t p_ops = p_stats.hits + p_stats.misses;
		double p_hit_rate = (p_ops > 0) ? (100.0 * p_stats.hits / p_ops) : 0.0;

		spdlog::info("[unified_cache]   P{:2d}: {:5d} entries ({:4.1f}%) | "
					 "{:6d} ops | hit: {:5.2f}%",
			i, entries, usage, p_ops, p_hit_rate);
	}
}

}  // namespace ezio
