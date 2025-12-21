#include <string>
#include <sys/mman.h>
#include <thread>
#include <chrono>

#include <boost/assert.hpp>
#include <spdlog/spdlog.h>
#include <libtorrent/hasher.hpp>

#include "raw_disk_io.hpp"
#include "buffer_pool.hpp"
#include "store_buffer.hpp"

namespace ezio
{

// Global pointer to raw_disk_io instance for stats reporting
// Set by raw_disk_io_constructor, accessed by log thread
static raw_disk_io *g_raw_disk_io_instance = nullptr;

// Helper function: Get cache entries from settings_pack::cache_size
// cache_size is number of 16KiB blocks (libtorrent definition)
// Returns number of 16KB entries
static size_t calculate_cache_entries(libtorrent::settings_interface const &sett)
{
	int cache_entries = sett.get_int(libtorrent::settings_pack::cache_size);

	// cache_size is already the number of 16KB blocks
	size_t entries = static_cast<size_t>(cache_entries);

	spdlog::info("[raw_disk_io] Cache size: {} entries ({} MB)",
		entries, (entries * 16) / 1024);

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
	auto disk_io = std::make_unique<raw_disk_io>(ioc, s, c);
	g_raw_disk_io_instance = disk_io.get();
	return disk_io;
}

raw_disk_io *get_raw_disk_io_instance()
{
	return g_raw_disk_io_instance;
}

raw_disk_io::raw_disk_io(libtorrent::io_context &ioc,
	libtorrent::settings_interface const &sett,
	libtorrent::counters &cnt) :
	ioc_(ioc),
	m_settings(&sett),
	m_stats_counters(cnt),
	m_buffer_pool(ioc),
	m_cache(calculate_cache_entries(sett)),	 // Initialize from settings_pack::cache_size
	num_io_threads_(sett.get_int(libtorrent::settings_pack::aio_threads))
{
	// Calculate entries per partition (cache is divided equally among threads)
	// NOTE: Don't use reserve() on io_thread_pools_ as thread_pool is not movable
	size_t cache_entries = calculate_cache_entries(sett);
	size_t entries_per_partition = cache_entries / num_io_threads_;

	spdlog::info("[raw_disk_io] Initializing {} I/O threads with consistent hashing",
		num_io_threads_);
	spdlog::info("[raw_disk_io] Each thread owns {} cache entries ({} MB)",
		entries_per_partition,
		entries_per_partition * DEFAULT_BLOCK_SIZE / (1024 * 1024));

	// Initialize cache partitions
	m_cache.resize_partitions(num_io_threads_, entries_per_partition);

	// Create each thread pool with 1 thread
	for (size_t i = 0; i < num_io_threads_; ++i) {
		io_thread_pools_.emplace_back(std::make_unique<boost::asio::thread_pool>(1));  // 1 thread per pool
		spdlog::debug("[raw_disk_io] I/O thread pool {} created", i);
	}

	spdlog::info("[raw_disk_io] All {} I/O thread pools started successfully", num_io_threads_);

	// Start stats reporting thread (posts tasks to each io thread - lock-free!)
	m_stats_thread = std::thread(&raw_disk_io::stats_report_loop, this);
}

raw_disk_io::~raw_disk_io()
{
	spdlog::info("[raw_disk_io] Shutting down: waiting for all I/O to complete...");

	// Stop stats reporting thread first
	m_shutdown = true;
	if (m_stats_thread.joinable()) {
		m_stats_thread.join();
	}

	// Join all thread pools (waits for all pending work to complete)
	for (size_t i = 0; i < num_io_threads_; ++i) {
		spdlog::debug("[raw_disk_io] Joining I/O thread pool {}...", i);
		io_thread_pools_[i]->join();  // Blocks until all work done
		spdlog::debug("[raw_disk_io] I/O thread pool {} joined", i);
	}

	spdlog::info("[raw_disk_io] All I/O threads stopped, shutdown complete");
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

	// Use consistent hashing to select thread (all cache ops on worker thread)
	size_t thread_idx = get_thread_index(idx, r.piece);

	// Post all work to worker thread (lock-free: single thread per partition)
	boost::asio::post((*io_thread_pools_[thread_idx]),
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
			libtorrent::storage_error error;

			if (read_offset + r.length > DEFAULT_BLOCK_SIZE) {
				// Unaligned - spans two blocks
				torrent_location const loc1{idx, r.piece, block_offset};
				torrent_location const loc2{idx, r.piece, block_offset + DEFAULT_BLOCK_SIZE};
				std::ptrdiff_t const len1 = DEFAULT_BLOCK_SIZE - read_offset;

				// Check cache on worker thread
				int ret = m_cache.get2(loc1, loc2, [&](char const *buf1, char const *buf2) {
					if (buf1) {
						std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
					}
					if (buf2) {
						std::memcpy(buf + len1, buf2, std::size_t(r.length - len1));
					}
					return (buf1 ? 2 : 0) | (buf2 ? 1 : 0);
				});

				if (ret != 3) {
					// Partial or complete miss - read from disk
					auto offset = (ret == 0) ? r.start : ((ret & 2) ? (block_offset + DEFAULT_BLOCK_SIZE) : r.start);
					auto len = (ret == 0) ? r.length : ((ret & 2) ? (r.length - len1) : len1);
					auto buf_offset = (ret == 0) ? 0 : ((ret & 2) ? len1 : 0);

					auto const start_time = libtorrent::clock_type::now();
					storages_[idx]->read(buf + buf_offset, r.piece, offset, len, error);
					auto const read_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

					m_stats_counters.inc_stats_counter(libtorrent::counters::num_read_ops);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_read_time, read_time);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, read_time);
				}

				m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read, 2);
			} else {
				// Aligned - single block
				bool cache_hit = m_cache.get({idx, r.piece, block_offset}, [&](char const *cache_buf) {
					std::memcpy(buf, cache_buf + read_offset, std::size_t(r.length));
				});

				if (!cache_hit) {
					// Cache miss - read from disk
					auto const start_time = libtorrent::clock_type::now();
					storages_[idx]->read(buf, r.piece, r.start, r.length, error);
					auto const read_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

					m_stats_counters.inc_stats_counter(libtorrent::counters::num_read_ops);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_read_time, read_time);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, read_time);

					// Insert into cache (clean) for future reads
					if (!error) {
						m_cache.insert_read({idx, r.piece, block_offset}, buf, r.length);
					}
				}

				m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read);
			}

			// Post result back to main thread
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

	char *temp_buf = m_buffer_pool.allocate_buffer();
	libtorrent::disk_buffer_holder buffer(m_buffer_pool, temp_buf, DEFAULT_BLOCK_SIZE);
	if (temp_buf) {
		// Copy data to temp buffer (buf may be freed by caller after we return)
		std::memcpy(temp_buf, buf, r.length);

		// Use consistent hashing to select thread (all cache ops on worker thread)
		size_t thread_idx = get_thread_index(storage, r.piece);

		// Post all work to worker thread (lock-free: single thread per partition)
		boost::asio::post((*io_thread_pools_[thread_idx]),
			[=, this, o = std::move(o), handler = std::move(handler), buffer = std::move(buffer)]() mutable {
				torrent_location loc{storage, r.piece, r.start};

				// Get temp buffer pointer
				char *temp_buf = buffer.data();

				// Insert into cache (cache allocates buffer and copies data)
				// All cache operations happen HERE on worker thread
				bool exceeded = false;
				bool cache_inserted = m_cache.insert_write(loc, temp_buf, r.length, exceeded, o);

				char *cache_buf = nullptr;
				if (cache_inserted) {
					// Get buffer pointer from cache
					m_cache.get(loc, [&](char const *data) {
						cache_buf = const_cast<char *>(data);
					});
				}

				libtorrent::storage_error error;
				auto const start_time = libtorrent::clock_type::now();

				if (cache_buf) {
					// Write-through: write to disk using cache buffer
					storages_[storage]->write(cache_buf, r.piece, r.start, r.length, error);

					// Mark cache entry as clean (write completed)
					m_cache.mark_clean(loc);
				} else {
					// Cache unavailable - write directly from temp buffer
					spdlog::debug("[async_write] Cache unavailable, writing from temp buffer (storage={}, piece={}, offset={})",
						static_cast<int>(storage), static_cast<int>(r.piece), r.start);
					storages_[storage]->write(temp_buf, r.piece, r.start, r.length, error);
				}

				auto const write_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

				// Update counters
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_written);
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_write_ops);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_write_time, write_time);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, write_time);

				// buffer destructor will return buffer to pool

				// Call handler
				post(ioc_, [=, h = std::move(handler)] {
					h(error);
				});
			});
		return false;  // Successfully queued
	}

	// Fallback: buffer pool exhausted - do sync write without cache
	spdlog::debug("[async_write] Buffer pool exhausted, doing sync write (storage={}, piece={}, offset={})",
		static_cast<int>(storage), static_cast<int>(r.piece), r.start);

	libtorrent::storage_error error;
	auto const start_time = libtorrent::clock_type::now();
	storages_[storage]->write(const_cast<char *>(buf), r.piece, r.start, r.length, error);
	auto const write_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

	// Update counters
	m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_written);
	m_stats_counters.inc_stats_counter(libtorrent::counters::num_write_ops);
	m_stats_counters.inc_stats_counter(libtorrent::counters::disk_write_time, write_time);
	m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, write_time);

	post(ioc_, [=, h = std::move(handler)] {
		h(error);
	});

	return false;
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

	// Use consistent hashing: hash operations use same thread as I/O for this piece
	// Since all blocks of a piece go to same partition, no cross-partition access needed
	size_t thread_idx = get_thread_index(storage, piece);
	boost::asio::post((*io_thread_pools_[thread_idx]),
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() {
			libtorrent::storage_error error;
			libtorrent::hasher ph;
			partition_storage *st = storages_[storage].get();

			auto const start_time = libtorrent::clock_type::now();

			int const piece_size = st->piece_size(piece);
			int const blocks_in_piece = (piece_size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

			int offset = 0;
			int const blocks_to_read = blocks_in_piece;
			int len = 0;
			int ret = 0;
			for (int i = 0; i < blocks_to_read; i++) {
				len = std::min(DEFAULT_BLOCK_SIZE, piece_size - offset);

				// Try cache
				bool hit = m_cache.get({storage, piece, offset}, [&](char const *cache_buf) {
					ph.update(cache_buf, len);
					ret = len;
				});

				// Read from disk if not in cache
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

			auto const hash_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

			// Update counters
			m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_hashed, blocks_in_piece);
			m_stats_counters.inc_stats_counter(libtorrent::counters::disk_hash_time, hash_time);
			m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, hash_time);

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
	// Update buffer pool usage (gauge)
	c.set_value(libtorrent::counters::disk_blocks_in_use, m_buffer_pool.in_use());

	// Update cache statistics (gauges)
	// Note: Could add custom cache metrics here if libtorrent adds counters for them
	// For now, we rely on the inc_stats_counter calls in async_read/write/hash
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

void raw_disk_io::stats_report_loop()
{
	spdlog::info("[raw_disk_io] Cache stats reporting thread started (30s interval)");

	while (!m_shutdown) {
		std::this_thread::sleep_for(std::chrono::seconds(30));

		if (m_shutdown) {
			break;
		}

		// Post stats logging task to each io thread
		// Each thread logs its own partition only (lock-free!)
		spdlog::info("[unified_cache] === Lock-Free Cache Performance Statistics ===");

		for (size_t i = 0; i < num_io_threads_; ++i) {
			boost::asio::post((*io_thread_pools_[i]), [this, i]() {
				// Each io thread reads its own partition only (lock-free!)
				auto p_stats = m_cache.get_partition_stats(i);	// Single partition
				size_t entries = m_cache.get_partition_size(i);
				size_t max_entries = m_cache.get_partition_max_entries(i);

				double usage = (max_entries > 0) ? (100.0 * entries / max_entries) : 0.0;
				uint64_t p_ops = p_stats.hits + p_stats.misses;
				double p_hit_rate = (p_ops > 0) ? (100.0 * p_stats.hits / p_ops) : 0.0;

				spdlog::info("[unified_cache]   P{:2d}: {:5d} entries ({:4.1f}%) | "
							 "{:6d} ops | hit: {:5.2f}%",
					i, entries, usage, p_ops, p_hit_rate);
			});
		}
	}

	spdlog::info("[raw_disk_io] Cache stats reporting thread exiting");
}


}  // namespace ezio
