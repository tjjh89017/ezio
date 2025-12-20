#include <string>
#include <sys/mman.h>

#include <boost/assert.hpp>
#include <spdlog/spdlog.h>
#include <libtorrent/hasher.hpp>

#include "raw_disk_io.hpp"
#include "buffer_pool.hpp"
#include "store_buffer.hpp"

namespace ezio
{

// Flush interval for time-based dirty cache flush (seconds)
// Default 120s allows more write coalescing and reduces I/O frequency
// Can be adjusted based on workload:
// - Lower (30-60s): More frequent flush, better for memory pressure
// - Higher (180-300s): Better write coalescing, requires more memory
constexpr int CACHE_FLUSH_INTERVAL_SECONDS = 120;

// Helper function: Calculate cache entries from settings_pack::cache_size
// cache_size unit is KiB (libtorrent convention)
// Returns number of 16KB entries
static size_t calculate_cache_entries(libtorrent::settings_interface const &sett)
{
	int cache_kb = sett.get_int(libtorrent::settings_pack::cache_size);

	// Convert KiB to bytes, then divide by 16KB per entry
	// Example: 512MB = 524288 KB -> (524288 * 1024) / 16384 = 32768 entries
	size_t entries = (static_cast<size_t>(cache_kb) * 1024) / 16384;

	spdlog::info("[raw_disk_io] Cache size: {} KB -> {} entries ({} MB)",
		cache_kb, entries, (entries * 16) / 1024);

	return entries;
}

class partition_storage
{
private:
	// fd to partition.
	int fd_{0};
	void *mapping_addr_{nullptr};
	size_t mapping_len_{0};

	libtorrent::file_storage const &fs_;

public:
	partition_storage(const std::string &path, libtorrent::file_storage const &fs) :
		fs_(fs)
	{
		fd_ = open(path.c_str(), O_RDWR);
		if (fd_ < 0) {
			spdlog::critical("failed to open ({}) = {}", path, strerror(fd_));
			exit(1);
		}
	}

	~partition_storage()
	{
		int ec = close(fd_);
		if (ec) {
			spdlog::error("close: {}", strerror(ec));
		}
	}

	int piece_size(libtorrent::piece_index_t const piece)
	{
		return fs_.piece_size(piece);
	}

	int read(char *buffer, libtorrent::piece_index_t const piece, int const offset,
		int const length, libtorrent::storage_error &error)
	{
		BOOST_ASSERT(buffer != nullptr);

		auto file_slices = fs_.map_block(piece, offset, length);
		int ret = 0;

		for (const auto &file_slice : file_slices) {
			const auto &file_index = file_slice.file_index;

			int64_t partition_offset = 0;

			// to find partition_offset from file name.
			std::string file_name(fs_.file_name(file_index));
			try {
				partition_offset = std::stoll(file_name, 0, 16);
				partition_offset += file_slice.offset;
			} catch (const std::exception &e) {
				spdlog::critical("failed to parse file_name({}) at ({}): {}",
					file_name, static_cast<std::int32_t>(file_index), e.what());
				error.file(file_index);
				error.ec = libtorrent::errors::parse_failed;
				error.operation = libtorrent::operation_t::file_read;
				return ret;
			}

			pread(fd_, buffer, file_slice.size, partition_offset);
			ret += file_slice.size;
			buffer += file_slice.size;
		}
		return ret;
	}

	void write(char *buffer, libtorrent::piece_index_t const piece, int const offset,
		int const length, libtorrent::storage_error &error)
	{
		BOOST_ASSERT(buffer != nullptr);

		auto file_slices = fs_.map_block(piece, offset, length);

		for (const auto &file_slice : file_slices) {
			const auto &file_index = file_slice.file_index;

			int64_t partition_offset = 0;

			// to find partition_offset from file name.
			std::string file_name(fs_.file_name(file_index));
			try {
				partition_offset = std::stoll(file_name, 0, 16);
				partition_offset += file_slice.offset;
			} catch (const std::exception &e) {
				spdlog::critical("failed to parse file_name({}) at ({}): {}",
					file_name, static_cast<std::int32_t>(file_index), e.what());
				error.file(file_index);
				error.ec = libtorrent::errors::parse_failed;
				error.operation = libtorrent::operation_t::file_write;
				return;
			}

			pwrite(fd_, buffer, file_slice.size, partition_offset);
			buffer += file_slice.size;
		}
	}
};

std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(libtorrent::io_context &ioc,
	libtorrent::settings_interface const &s,
	libtorrent::counters &c)
{
	return std::make_unique<raw_disk_io>(ioc, s, c);
}

raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
	libtorrent::settings_interface const &sett,
	libtorrent::counters &cnt) :
	ioc_(ioc),
	m_settings(&sett),
	m_stats_counters(cnt),
	m_buffer_pool(ioc),
	m_cache(calculate_cache_entries(sett)),	 // Initialize from settings_pack::cache_size
	read_thread_pool_(sett.get_int(libtorrent::settings_pack::aio_threads)),
	write_thread_pool_(sett.get_int(libtorrent::settings_pack::aio_threads)),
	hash_thread_pool_(sett.get_int(libtorrent::settings_pack::hashing_threads)),
	m_flush_timer(ioc)	// Initialize flush timer
{
	// Start periodic flush timer
	m_flush_timer.expires_after(std::chrono::seconds(CACHE_FLUSH_INTERVAL_SECONDS));
	m_flush_timer.async_wait([this](boost::system::error_code const &ec) {
		on_flush_timer(ec);
	});
}

raw_disk_io::~raw_disk_io()
{
	// Cancel flush timer
	m_flush_timer.cancel();

	read_thread_pool_.join();
	write_thread_pool_.join();
	hash_thread_pool_.join();
}

libtorrent::storage_holder raw_disk_io::new_torrent(libtorrent::storage_params const &p,
	std::shared_ptr<void> const &)
{
	const std::string &target_partition = p.path;

	int idx = storages_.size();
	if (!free_slots_.empty()) {
		// TODO need a lock
		idx = free_slots_.front();
		free_slots_.pop_front();
	}

	auto storage = std::make_unique<partition_storage>(target_partition, p.files);
	storages_.emplace(idx, std::move(storage));

	if (idx > 0) {
		spdlog::warn("new_torrent current idx => {}, should be 0", idx);
	}

	return libtorrent::storage_holder(idx, *this);
}

void raw_disk_io::remove_torrent(libtorrent::storage_index_t idx)
{
	// TODO need a lock
	storages_.erase(idx);
	free_slots_.push_back(idx);
}

void raw_disk_io::async_read(
	libtorrent::storage_index_t idx, libtorrent::peer_request const &r,
	std::function<void(libtorrent::disk_buffer_holder, libtorrent::storage_error const &)> handler,
	libtorrent::disk_job_flags_t flags)
{
	BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

	libtorrent::storage_error error;
	if (r.length <= 0 || r.start < 0) {
		error.ec = libtorrent::errors::invalid_request;
		error.operation = libtorrent::operation_t::file_read;
		handler(libtorrent::disk_buffer_holder{}, error);
		return;
	}

	char *buf = m_buffer_pool.allocate_buffer();
	libtorrent::disk_buffer_holder buffer(m_buffer_pool, buf, DEFAULT_BLOCK_SIZE);
	if (!buf) {
		error.ec = libtorrent::errors::no_memory;
		error.operation = libtorrent::operation_t::alloc_cache_piece;
		handler(libtorrent::disk_buffer_holder{}, error);
		return;
	}

	int const block_offset = r.start - (r.start % DEFAULT_BLOCK_SIZE);
	int const read_offset = r.start - block_offset;
	// it might be unaligned request, refer libtorrent async_read

	if (read_offset + r.length > DEFAULT_BLOCK_SIZE) {
		// unaligned - spans two blocks
		torrent_location const loc1{idx, r.piece, block_offset};
		torrent_location const loc2{idx, r.piece, block_offset + DEFAULT_BLOCK_SIZE};
		std::ptrdiff_t const len1 = DEFAULT_BLOCK_SIZE - read_offset;

		BOOST_ASSERT(r.length > len1);

		// 1. Check store_buffer first (highest priority - data being written)
		// ret encoding: (buf1 ? 2 : 0) | (buf2 ? 1 : 0)
		//   0 = neither, 1 = buf2 only, 2 = buf1 only, 3 = both
		int ret = m_store_buffer.get2(loc1, loc2, [&](char const *buf1, char const *buf2) {
			if (buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
			}
			if (buf2) {
				std::memcpy(buf + len1, buf2, std::size_t(r.length - len1));
			}
			return (buf1 ? 2 : 0) | (buf2 ? 1 : 0);
		});

		if (ret == 3) {
			// Both blocks found in store_buffer
			handler(std::move(buffer), error);
			return;
		}

		// 2. Check cache for missing blocks (persistent cache)
		int const ret_cache = m_cache.get2(loc1, loc2, [&](char const *buf1, char const *buf2) {
			// Only copy blocks not already found in store_buffer
			if (buf1 && !(ret & 2)) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
			}
			if (buf2 && !(ret & 1)) {
				std::memcpy(buf + len1, buf2, std::size_t(r.length - len1));
			}
			return (buf1 ? 2 : 0) | (buf2 ? 1 : 0);
		});

		// Merge cache results with store_buffer results
		ret |= ret_cache;

		if (ret == 3) {
			// Both blocks now found (from store_buffer + cache)
			handler(std::move(buffer), error);
			return;
		}

		// 3. Partial hit - read missing block(s) from disk
		// Note: For unaligned reads, we don't insert into cache (data not block-aligned)
		if (ret != 0) {
			boost::asio::post(read_thread_pool_,
				[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
					libtorrent::storage_error error;

					// Determine which block to read
					auto offset = (ret & 2) ? (block_offset + DEFAULT_BLOCK_SIZE) : r.start;
					auto len = (ret & 2) ? (r.length - len1) : len1;
					auto buf_offset = (ret & 2) ? len1 : 0;

					storages_[idx]->read(buf + buf_offset, r.piece, offset, len, error);

					post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
						h(std::move(b), error);
					});
				});
			return;
		}

		// 4. Full cache miss - read from disk (unaligned case, no cache insertion)
		boost::asio::post(read_thread_pool_,
			[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
				libtorrent::storage_error error;
				storages_[idx]->read(buf, r.piece, r.start, r.length, error);

				post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
					h(std::move(b), error);
				});
			});
		return;
	} else {
		// aligned block
		// 1. Check store_buffer first (highest priority - data being written)
		if (m_store_buffer.get({idx, r.piece, block_offset}, [&](char const *buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(r.length));
			})) {
			handler(std::move(buffer), error);
			return;
		}

		// 2. Check cache second (persistent cache)
		if (m_cache.get({idx, r.piece, block_offset}, [&](char const *buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(r.length));
			})) {
			handler(std::move(buffer), error);
			return;
		}
	}

	// 3. Cache miss - read from disk
	boost::asio::post(read_thread_pool_,
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
			libtorrent::storage_error error;
			storages_[idx]->read(buf, r.piece, r.start, r.length, error);

			// Insert into cache (clean) for future reads
			if (!error) {
				m_cache.insert_read({idx, r.piece, block_offset}, buf);
			}

			post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
				h(std::move(b), error);
			});
		});
}

bool raw_disk_io::async_write(libtorrent::storage_index_t storage, libtorrent::peer_request const &r,
	char const *buf, std::shared_ptr<libtorrent::disk_observer> o,
	std::function<void(libtorrent::storage_error const &)> handler,
	libtorrent::disk_job_flags_t flags)
{
	BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

	bool exceeded = false;
	libtorrent::disk_buffer_holder buffer(m_buffer_pool, m_buffer_pool.allocate_buffer(exceeded, o), DEFAULT_BLOCK_SIZE);

	if (buffer) {
		// async
		memcpy(buffer.data(), buf, r.length);

		// Insert into store_buffer (temporary cache for async_read before write completes)
		m_store_buffer.insert({storage, r.piece, r.start}, buffer.data());

		// Insert into unified_cache (persistent cache, marked dirty)
		m_cache.insert_write({storage, r.piece, r.start}, buffer.data());

		// Check if cache needs flushing (opportunistic flush)
		// Phase 3.1: Still writes immediately, but prepares for Phase 3.2 delayed write
		if (should_flush_dirty_cache()) {
			// Post flush job to write thread pool (non-blocking)
			boost::asio::post(write_thread_pool_, [this, storage]() {
				flush_dirty_blocks(storage);
			});
		}

		libtorrent::peer_request r2(r);
		boost::asio::post(write_thread_pool_,
			[=, this, handler = std::move(handler), buffer = std::move(buffer)]() {
				libtorrent::storage_error error;

				// Write to disk
				storages_[storage]->write(buffer.data(), r.piece, r.start, r.length, error);

				// After write completes:
				// 1. Remove from store_buffer (free temp buffer)
				m_store_buffer.erase({storage, r.piece, r.start});

				// 2. Mark cache entry as clean (but keep in cache!)
				m_cache.mark_clean({storage, r.piece, r.start});

				post(ioc_, [=, h = std::move(handler)] {
					h(error);
				});
			});
		return exceeded;
	}

	// sync
	libtorrent::storage_error error;
	storages_[storage]->write(const_cast<char *>(buf), r.piece, r.start, r.length, error);

	post(ioc_, [=, h = std::move(handler)] {
		h(error);
	});
	return exceeded;
}

void raw_disk_io::async_hash(
	libtorrent::storage_index_t storage, libtorrent::piece_index_t piece, libtorrent::span<libtorrent::sha256_hash> v2,
	libtorrent::disk_job_flags_t flags,
	std::function<void(libtorrent::piece_index_t, libtorrent::sha1_hash const &, libtorrent::storage_error const &)>
		handler)
{
	libtorrent::storage_error error;
	char *buf = m_buffer_pool.allocate_buffer();

	if (!buf) {
		error.ec = libtorrent::errors::no_memory;
		error.operation = libtorrent::operation_t::alloc_cache_piece;
		post(ioc_, [=, h = std::move(handler)] {
			h(piece, libtorrent::sha1_hash{}, error);
		});
		return;
	}

	auto buffer = libtorrent::disk_buffer_holder(m_buffer_pool, buf, DEFAULT_BLOCK_SIZE);

	boost::asio::post(hash_thread_pool_,
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() {
			libtorrent::storage_error error;
			libtorrent::hasher ph;
			partition_storage *st = storages_[storage].get();

			int const piece_size = st->piece_size(piece);
			int const blocks_in_piece = (piece_size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

			int offset = 0;
			int const blocks_to_read = blocks_in_piece;
			int len = 0;
			int ret = 0;
			for (int i = 0; i < blocks_to_read; i++) {
				len = std::min(DEFAULT_BLOCK_SIZE, piece_size - offset);
				bool hit = m_store_buffer.get({storage, piece, offset}, [&](char const *buf1) {
					ph.update(buf1, len);
					ret = len;
				});
				if (!hit) {
					ret = st->read(buf, piece, offset, len, error);
					if (ret > 0) {
						ph.update(buf, ret);
					}
				}
				if (ret <= 0) {
					break;
				}
				offset += ret;
			}

			libtorrent::sha1_hash const hash = ph.final();

			post(ioc_, [=, h = std::move(handler)] {
				h(piece, hash, error);
			});
		});
}

void raw_disk_io::async_hash2(
	libtorrent::storage_index_t storage, libtorrent::piece_index_t piece, int offset,
	libtorrent::disk_job_flags_t flags,
	std::function<void(libtorrent::piece_index_t, libtorrent::sha256_hash const &, libtorrent::storage_error const &)>
		handler)
{
}

void raw_disk_io::async_move_storage(
	libtorrent::storage_index_t storage, std::string p, libtorrent::move_flags_t flags,
	std::function<void(libtorrent::status_t, std::string const &, libtorrent::storage_error const &)>
		handler)
{
	post(ioc_, [=] {
		handler(libtorrent::status_t::fatal_disk_error, p,
			libtorrent::storage_error(
				libtorrent::error_code(boost::system::errc::operation_not_supported, libtorrent::system_category())));
	});
}

void raw_disk_io::async_release_files(libtorrent::storage_index_t storage,
	std::function<void()> handler)
{
}

void raw_disk_io::async_check_files(
	libtorrent::storage_index_t storage, libtorrent::add_torrent_params const *resume_data,
	libtorrent::aux::vector<std::string, libtorrent::file_index_t> links,
	std::function<void(libtorrent::status_t, libtorrent::storage_error const &)> handler)
{
	post(ioc_, [=] {
		handler(libtorrent::status_t::no_error, libtorrent::storage_error());
	});
}

void raw_disk_io::async_stop_torrent(libtorrent::storage_index_t storage,
	std::function<void()> handler)
{
	post(ioc_, handler);
}

void raw_disk_io::async_rename_file(
	libtorrent::storage_index_t storage, libtorrent::file_index_t index, std::string name,
	std::function<void(std::string const &, libtorrent::file_index_t, libtorrent::storage_error const &)>
		handler)
{
	post(ioc_, [=] {
		handler(name, index, libtorrent::storage_error());
	});
}

void raw_disk_io::async_delete_files(
	libtorrent::storage_index_t storage, libtorrent::remove_flags_t options,
	std::function<void(libtorrent::storage_error const &)> handler)
{
	post(ioc_, [=] {
		handler(libtorrent::storage_error());
	});
}

void raw_disk_io::async_set_file_priority(
	libtorrent::storage_index_t storage, libtorrent::aux::vector<libtorrent::download_priority_t, libtorrent::file_index_t> prio,
	std::function<void(libtorrent::storage_error const &,
		libtorrent::aux::vector<libtorrent::download_priority_t, libtorrent::file_index_t>)>
		handler)
{
	post(ioc_, [=] {
		handler(libtorrent::storage_error(libtorrent::error_code(
					boost::system::errc::operation_not_supported, libtorrent::system_category())),
			std::move(prio));
	});
}

void raw_disk_io::async_clear_piece(libtorrent::storage_index_t storage,
	libtorrent::piece_index_t index,
	std::function<void(libtorrent::piece_index_t)> handler)
{
	post(ioc_, [=] {
		handler(index);
	});
}

void raw_disk_io::update_stats_counters(libtorrent::counters &c) const
{
}

std::vector<libtorrent::open_file_state> raw_disk_io::get_status(libtorrent::storage_index_t) const
{
	return {};
}

void raw_disk_io::abort(bool wait)
{
}

void raw_disk_io::submit_jobs()
{
}

void raw_disk_io::settings_updated()
{
	// Update cache size from settings
	size_t new_cache_entries = calculate_cache_entries(*m_settings);
	if (new_cache_entries != m_cache.max_entries()) {
		spdlog::info("[raw_disk_io] Updating cache size from {} to {} entries ({} MB -> {} MB)",
			m_cache.max_entries(), new_cache_entries,
			(m_cache.max_entries() * 16) / 1024, (new_cache_entries * 16) / 1024);
		m_cache.set_max_entries(new_cache_entries);
	}

	// Note: Thread pool sizes are set in constructor init list and cannot be changed at runtime
	// Future: Consider implementing dynamic thread pool resizing if needed
}

// ============================================================================
// Delayed Write Implementation (Phase 3.1.1)
// ============================================================================

bool raw_disk_io::should_flush_dirty_cache() const
{
	// Flush trigger 1: Cache usage exceeds threshold (80%)
	int usage = m_cache.usage_percentage();	 // 0-100
	if (usage > 80) {
		return true;
	}

	// Flush trigger 2: Dirty block count exceeds threshold
	// For 512MB cache (32768 entries), flush if > 8192 dirty blocks (25%)
	size_t dirty_count = m_cache.total_dirty_count();
	size_t dirty_threshold = m_cache.max_entries() / 4;	 // 25% dirty threshold
	if (dirty_count > dirty_threshold) {
		return true;
	}

	// No flush needed
	return false;
}

void raw_disk_io::flush_dirty_blocks(libtorrent::storage_index_t storage)
{
	// Collect all dirty blocks for this storage
	std::vector<torrent_location> dirty_blocks = m_cache.collect_dirty_blocks(storage);

	if (dirty_blocks.empty()) {
		return;
	}

	spdlog::debug("[raw_disk_io] Flushing {} dirty blocks for storage {}",
		dirty_blocks.size(), static_cast<int>(storage));

	// Sort by disk offset for sequential writes
	// This reduces seek time on HDD and improves write throughput
	std::sort(dirty_blocks.begin(), dirty_blocks.end(), [](torrent_location const &a, torrent_location const &b) {
		// Compare by piece first, then offset
		if (a.piece != b.piece) {
			return a.piece < b.piece;
		}
		return a.offset < b.offset;
	});

	// Write each dirty block to disk
	// Phase 3.2 (future): Use pwritev() for write coalescing
	//
	// Thread safety: collect_dirty_blocks() atomically marks blocks as flushing,
	// which pins them to cache and prevents eviction. Concurrent writes are allowed
	// and will set dirty=true, which mark_clean_if_flushing() will detect.
	for (auto const &loc : dirty_blocks) {
		// Get block data from cache (already marked flushing by collect_dirty_blocks)
		char temp_buf[DEFAULT_BLOCK_SIZE];
		bool found = m_cache.get(loc, [&](char const *buf) {
			memcpy(temp_buf, buf, DEFAULT_BLOCK_SIZE);
		});

		if (!found) {
			// Block was evicted (shouldn't happen since flushing=true pins it)
			spdlog::warn("[raw_disk_io] Flushing block disappeared: piece={} offset={}",
				static_cast<int>(loc.piece), loc.offset);
			m_cache.set_flushing(loc, false);
			continue;
		}

		// Write to disk (cache is unlocked, allows concurrent operations)
		libtorrent::storage_error error;
		storages_[storage]->write(temp_buf, loc.piece, loc.offset, DEFAULT_BLOCK_SIZE, error);

		if (!error) {
			// Atomically mark clean only if not modified during flush
			// If concurrent write happened (dirty=true), keep it dirty for next flush
			bool marked_clean = m_cache.mark_clean_if_flushing(loc);
			if (!marked_clean) {
				spdlog::debug("[raw_disk_io] Block modified during flush, keeping dirty: piece={} offset={}",
					static_cast<int>(loc.piece), loc.offset);
			}
		} else {
			// Write failed, clear flushing flag but keep dirty
			m_cache.set_flushing(loc, false);
			spdlog::error("[raw_disk_io] Failed to flush block piece={} offset={}: {}",
				static_cast<int>(loc.piece), loc.offset, error.ec.message());
		}
	}

	spdlog::debug("[raw_disk_io] Flushed {} dirty blocks for storage {}",
		dirty_blocks.size(), static_cast<int>(storage));
}

void raw_disk_io::on_flush_timer(boost::system::error_code const &ec)
{
	if (ec) {
		// Timer was cancelled (likely during shutdown)
		return;
	}

	// Check if we should flush dirty cache
	if (should_flush_dirty_cache()) {
		spdlog::debug("[raw_disk_io] Time-based flush triggered (usage: {}%, dirty: {})",
			m_cache.usage_percentage(), m_cache.total_dirty_count());

		// Post flush jobs to write thread pool (non-blocking, avoids blocking io_context)
		for (auto const &pair : storages_) {
			libtorrent::storage_index_t storage_id = pair.first;
			boost::asio::post(write_thread_pool_, [this, storage_id]() {
				flush_dirty_blocks(storage_id);
			});
		}
	}

	// Reschedule timer for next flush
	m_flush_timer.expires_after(std::chrono::seconds(CACHE_FLUSH_INTERVAL_SECONDS));
	m_flush_timer.async_wait([this](boost::system::error_code const &ec) {
		on_flush_timer(ec);
	});
}

}  // namespace ezio
