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

		// State transition based on dirty flag
		bool was_dirty = it->second.dirty;
		it->second.dirty = dirty;

		// Determine target state
		cache_state new_state;
		if (dirty) {
			new_state = cache_state::write_lru;
			if (!was_dirty) {
				// Clean → dirty transition
				m_num_clean--;
				m_num_dirty++;
			}
		} else {
			// Read insert (cache miss fill) - goes to read_lru1
			new_state = cache_state::read_lru1;
			if (was_dirty) {
				// Dirty → clean transition
				m_num_dirty--;
				m_num_clean++;
			}
		}

		// Move to appropriate LRU list if state changed
		if (it->second.state != new_state) {
			move_to_list(it->second, new_state);
		} else {
			// Touch within same list (move to front)
			std::list<torrent_location> *target_list = nullptr;
			lru_stats_internal *target_stats = nullptr;

			switch (it->second.state) {
			case cache_state::write_lru:
				target_list = &m_write_lru;
				target_stats = &m_write_lru_stats;
				break;
			case cache_state::read_lru1:
				target_list = &m_read_lru1;
				target_stats = &m_read_lru1_stats;
				break;
			case cache_state::read_lru2:
				target_list = &m_read_lru2;
				target_stats = &m_read_lru2_stats;
				break;
			}

			// Move to front of current list
			target_list->erase(it->second.lru_iter);
			target_list->push_front(loc);
			it->second.lru_iter = target_list->begin();
			target_stats->hits++;
		}

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

	// Check watermark recovery after eviction (but before releasing lock)
	std::vector<std::weak_ptr<libtorrent::disk_observer>> observers_to_notify;
	if (did_evict) {
		observers_to_notify = check_watermark_recovery();
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

	// Determine which LRU list based on dirty flag
	std::list<torrent_location> *target_list = nullptr;
	lru_stats_internal *target_stats = nullptr;

	if (dirty) {
		// Write operation → write_lru (dirty blocks)
		entry.state = cache_state::write_lru;
		target_list = &m_write_lru;
		target_stats = &m_write_lru_stats;
		m_num_dirty++;
	} else {
		// Read operation (cache miss fill) → read_lru1 (accessed once)
		entry.state = cache_state::read_lru1;
		target_list = &m_read_lru1;
		target_stats = &m_read_lru1_stats;
		m_num_clean++;
	}

	// Add to appropriate LRU list (most recently used = front)
	target_list->push_front(loc);
	entry.lru_iter = target_list->begin();

	// Insert into map
	m_entries.emplace(loc, std::move(entry));

	m_stats.inserts++;
	target_stats->inserts++;

	// Release lock before notifying observers
	l.unlock();

	// Notify observers if we evicted and recovered
	for (auto &weak_obs : observers_to_notify) {
		if (auto obs = weak_obs.lock()) {
			obs->on_disk();
		}
	}

	return true;
}

void cache_partition::mark_clean(torrent_location const &loc)
{
	std::unique_lock<std::mutex> l(m_mutex);

	auto it = m_entries.find(loc);
	if (it != m_entries.end() && it->second.dirty) {
		it->second.dirty = false;

		// State transition: write_lru → read_lru2
		// Assumption: blocks that were just written are likely to be read soon
		// Put them in high-priority read cache (read_lru2)
		if (it->second.state == cache_state::write_lru) {
			move_to_list(it->second, cache_state::read_lru2);

			// Update counters
			m_num_dirty--;
			m_num_clean++;
		}

		// Check watermark recovery (dirty decreased)
		auto observers = check_watermark_recovery();
		l.unlock();

		// Notify observers without holding lock
		for (auto &weak_obs : observers) {
			if (auto obs = weak_obs.lock()) {
				obs->on_disk();
			}
		}

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

	// Multi-LRU eviction: O(1) operation!
	// Try read_lru1 first (lowest priority - accessed once)
	if (!m_read_lru1.empty()) {
		torrent_location const &loc = m_read_lru1.back();  // LRU end
		auto it = m_entries.find(loc);

		if (it == m_entries.end()) {
			spdlog::error("[cache_partition] LRU1 inconsistency: entry not found");
			return false;
		}

		// Free buffer and erase entry
		// Note: cache_entry destructor will free buffer via free()
		m_entries.erase(it);
		m_read_lru1.pop_back();

		// Update counters
		m_num_clean--;
		m_read_lru1_stats.evictions++;

		// Note: Caller should check watermark recovery after eviction
		return true;  // O(1) eviction!
	}

	// Try read_lru2 if lru1 is empty (higher priority - accessed 2+ times)
	if (!m_read_lru2.empty()) {
		torrent_location const &loc = m_read_lru2.back();  // LRU end
		auto it = m_entries.find(loc);

		if (it == m_entries.end()) {
			spdlog::error("[cache_partition] LRU2 inconsistency: entry not found");
			return false;
		}

		// Free buffer and erase entry
		m_entries.erase(it);
		m_read_lru2.pop_back();

		// Update counters
		m_num_clean--;
		m_read_lru2_stats.evictions++;

		// Note: Caller should check watermark recovery after eviction
		return true;  // O(1) eviction!
	}

	// All entries are dirty (in write_lru) - cannot evict
	// This is expected behavior when cache is under write pressure
	return false;
}

void cache_partition::touch(torrent_location const &loc)
{
	// NOTE: Caller must hold m_mutex!
	// This function is no longer used with multi-LRU design
	// State-specific touch logic is now in insert() and get()
	(void)loc;	// Suppress unused parameter warning
}

void cache_partition::move_to_list(cache_entry &entry, cache_state new_state)
{
	// NOTE: Caller must hold m_mutex!

	if (entry.state == new_state) {
		return;	 // Already in target list
	}

	// Remove from current LRU list
	std::list<torrent_location> *old_list = nullptr;
	lru_stats_internal *old_stats = nullptr;

	switch (entry.state) {
	case cache_state::write_lru:
		old_list = &m_write_lru;
		old_stats = &m_write_lru_stats;
		break;
	case cache_state::read_lru1:
		old_list = &m_read_lru1;
		old_stats = &m_read_lru1_stats;
		break;
	case cache_state::read_lru2:
		old_list = &m_read_lru2;
		old_stats = &m_read_lru2_stats;
		break;
	}

	old_list->erase(entry.lru_iter);

	// Add to new LRU list (at front = most recently used)
	std::list<torrent_location> *new_list = nullptr;
	lru_stats_internal *new_stats = nullptr;

	switch (new_state) {
	case cache_state::write_lru:
		new_list = &m_write_lru;
		new_stats = &m_write_lru_stats;
		break;
	case cache_state::read_lru1:
		new_list = &m_read_lru1;
		new_stats = &m_read_lru1_stats;
		break;
	case cache_state::read_lru2:
		new_list = &m_read_lru2;
		new_stats = &m_read_lru2_stats;
		break;
	}

	new_list->push_front(entry.loc);
	entry.lru_iter = new_list->begin();
	entry.state = new_state;

	// Update statistics (promotion/demotion)
	if (new_state == cache_state::read_lru2 &&
		(entry.state == cache_state::read_lru1 || entry.state == cache_state::write_lru)) {
		// Promotion to read_lru2
		old_stats->promotions++;
		new_stats->inserts++;
	} else {
		// Other transitions (just track inserts)
		new_stats->inserts++;
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

bool unified_cache::insert_write(torrent_location const &loc, char const *data, int length, bool &exceeded,
	std::shared_ptr<libtorrent::disk_observer> o)
{
	size_t partition_idx = get_partition_index(loc);

	// Try to insert (allows over-allocation like libtorrent 2.x)
	bool success = m_partitions[partition_idx].insert(loc, data, length, true);	 // dirty=true

	// Check watermark after insert (saves observer if exceeded)
	exceeded = !m_partitions[partition_idx].check_watermark(o);

	return success;
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

	spdlog::info("[unified_cache] === Multi-LRU Cache Performance Statistics ===");
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

	// Aggregate per-LRU statistics
	spdlog::info("[unified_cache] === Per-LRU-List Statistics (All Partitions) ===");

	uint64_t total_write_lru_size = 0, total_write_lru_inserts = 0, total_write_lru_hits = 0;
	uint64_t total_write_lru_promotions = 0, total_write_lru_evictions = 0;

	uint64_t total_read_lru1_size = 0, total_read_lru1_inserts = 0, total_read_lru1_hits = 0;
	uint64_t total_read_lru1_promotions = 0, total_read_lru1_evictions = 0;

	uint64_t total_read_lru2_size = 0, total_read_lru2_inserts = 0, total_read_lru2_hits = 0;
	uint64_t total_read_lru2_promotions = 0, total_read_lru2_evictions = 0;

	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		auto w_stats = m_partitions[i].get_write_lru_stats();
		auto r1_stats = m_partitions[i].get_read_lru1_stats();
		auto r2_stats = m_partitions[i].get_read_lru2_stats();

		total_write_lru_size += w_stats.size;
		total_write_lru_inserts += w_stats.inserts;
		total_write_lru_hits += w_stats.hits;
		total_write_lru_promotions += w_stats.promotions;
		total_write_lru_evictions += w_stats.evictions;

		total_read_lru1_size += r1_stats.size;
		total_read_lru1_inserts += r1_stats.inserts;
		total_read_lru1_hits += r1_stats.hits;
		total_read_lru1_promotions += r1_stats.promotions;
		total_read_lru1_evictions += r1_stats.evictions;

		total_read_lru2_size += r2_stats.size;
		total_read_lru2_inserts += r2_stats.inserts;
		total_read_lru2_hits += r2_stats.hits;
		total_read_lru2_promotions += r2_stats.promotions;
		total_read_lru2_evictions += r2_stats.evictions;
	}

	size_t total_ent = total_entries();
	double w_pct = (total_ent > 0) ? (100.0 * total_write_lru_size / total_ent) : 0.0;
	double r1_pct = (total_ent > 0) ? (100.0 * total_read_lru1_size / total_ent) : 0.0;
	double r2_pct = (total_ent > 0) ? (100.0 * total_read_lru2_size / total_ent) : 0.0;

	spdlog::info("[unified_cache]   write_lru: {:6d} entries ({:5.1f}%) | {:6d} inserts | {:6d} hits | {:6d} → lru2 | {:6d} evictions",
		total_write_lru_size, w_pct, total_write_lru_inserts, total_write_lru_hits,
		total_write_lru_promotions, total_write_lru_evictions);

	spdlog::info("[unified_cache]   read_lru1: {:6d} entries ({:5.1f}%) | {:6d} inserts | {:6d} hits | {:6d} promoted | {:6d} evictions",
		total_read_lru1_size, r1_pct, total_read_lru1_inserts, total_read_lru1_hits,
		total_read_lru1_promotions, total_read_lru1_evictions);

	spdlog::info("[unified_cache]   read_lru2: {:6d} entries ({:5.1f}%) | {:6d} inserts | {:6d} hits | {:6d} promoted | {:6d} evictions",
		total_read_lru2_size, r2_pct, total_read_lru2_inserts, total_read_lru2_hits,
		total_read_lru2_promotions, total_read_lru2_evictions);

	// Watermark status
	spdlog::info("[unified_cache] === Watermark Status ===");
	size_t exceeded_partitions = 0;
	float max_dirty_ratio = 0.0f;

	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		if (m_partitions[i].is_exceeded()) {
			exceeded_partitions++;
		}
		float ratio = m_partitions[i].get_dirty_ratio();
		if (ratio > max_dirty_ratio) {
			max_dirty_ratio = ratio;
		}
	}

	spdlog::info("[unified_cache]   Exceeded partitions: {} / {}", exceeded_partitions, NUM_PARTITIONS);
	spdlog::info("[unified_cache]   Max dirty ratio: {:.1f}%", max_dirty_ratio * 100.0);
	spdlog::info("[unified_cache]   Global dirty ratio: {:.1f}%",
		(total_ent > 0) ? (100.0 * total_dirty_count() / total_ent) : 0.0);

	// Per-partition distribution (condensed)
	spdlog::info("[unified_cache] === Per-Partition Distribution ===");
	for (size_t i = 0; i < NUM_PARTITIONS; ++i) {
		size_t entries = m_partitions[i].size();
		size_t max_entries = m_partitions[i].max_entries();
		auto p_stats = m_partitions[i].get_stats();

		double usage = (max_entries > 0) ? (100.0 * entries / max_entries) : 0.0;
		uint64_t p_ops = p_stats.hits + p_stats.misses;
		double p_hit_rate = (p_ops > 0) ? (100.0 * p_stats.hits / p_ops) : 0.0;

		spdlog::info("[unified_cache]   P{:2d}: {:5d} entries ({:4.1f}%) | "
					 "{:6d} ops | hit: {:5.2f}% | cont: {:4d}",
			i, entries, usage, p_ops, p_hit_rate, p_stats.lock_contentions);
	}
}

}  // namespace ezio
