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
	hash_thread_pool_(sett.get_int(libtorrent::settings_pack::hashing_threads))
{
}

raw_disk_io::~raw_disk_io()
{
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

	// Post all work (cache lookup + disk read) to read_thread_pool
	// This keeps main thread lightweight for network I/O
	boost::asio::post(read_thread_pool_,
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
			libtorrent::storage_error error;

			if (read_offset + r.length > DEFAULT_BLOCK_SIZE) {
				// unaligned - spans two blocks
				torrent_location const loc1{idx, r.piece, block_offset};
				torrent_location const loc2{idx, r.piece, block_offset + DEFAULT_BLOCK_SIZE};
				std::ptrdiff_t const len1 = DEFAULT_BLOCK_SIZE - read_offset;

				BOOST_ASSERT(r.length > len1);

				// 1. Check cache
				// ret encoding: (buf1 ? 2 : 0) | (buf2 ? 1 : 0)
				//   0 = neither, 1 = buf2 only, 2 = buf1 only, 3 = both
				int ret = m_cache.get2(loc1, loc2, [&](char const *buf1, char const *buf2) {
					if (buf1) {
						std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
					}
					if (buf2) {
						std::memcpy(buf + len1, buf2, std::size_t(r.length - len1));
					}
					return (buf1 ? 2 : 0) | (buf2 ? 1 : 0);
				});

				if (ret == 3) {
					// Both blocks found in cache
					m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read, 2);
					post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
						h(std::move(b), error);
					});
					return;
				}

				// 2. Partial or complete miss - read from disk
				// Determine which block(s) to read
				auto offset = (ret == 0) ? r.start : ((ret & 2) ? (block_offset + DEFAULT_BLOCK_SIZE) : r.start);
				auto len = (ret == 0) ? r.length : ((ret & 2) ? (r.length - len1) : len1);
				auto buf_offset = (ret == 0) ? 0 : ((ret & 2) ? len1 : 0);

				auto const start_time = libtorrent::clock_type::now();
				storages_[idx]->read(buf + buf_offset, r.piece, offset, len, error);
				auto const read_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

				// Update counters
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read, 2);
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_read_ops);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_read_time, read_time);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, read_time);

				post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
					h(std::move(b), error);
				});
			} else {
				// aligned block
				// 1. Check cache
				bool cache_hit = m_cache.get({idx, r.piece, block_offset}, [&](char const *buf1) {
					std::memcpy(buf, buf1 + read_offset, std::size_t(r.length));
				});

				if (cache_hit) {
					// Cache hit
					m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read);
					post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
						h(std::move(b), error);
					});
					return;
				}

				// 2. Cache miss - read from disk
				auto const start_time = libtorrent::clock_type::now();
				storages_[idx]->read(buf, r.piece, r.start, r.length, error);
				auto const read_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

				// Update counters
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_read);
				m_stats_counters.inc_stats_counter(libtorrent::counters::num_read_ops);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_read_time, read_time);
				m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, read_time);

				// Insert into cache (clean) for future reads
				if (!error) {
					m_cache.insert_read({idx, r.piece, block_offset}, buf, r.length);
				}

				post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
					h(std::move(b), error);
				});
			}
		});
}

bool raw_disk_io::async_write(libtorrent::storage_index_t storage, libtorrent::peer_request const &r,
	char const *buf, std::shared_ptr<libtorrent::disk_observer> o,
	std::function<void(libtorrent::storage_error const &)> handler,
	libtorrent::disk_job_flags_t flags)
{
	BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

	// Insert into cache (cache allocates buffer and copies data)
	// Marked as dirty to prevent eviction during write
	torrent_location loc{storage, r.piece, r.start};
	bool cache_inserted = m_cache.insert_write(loc, buf, r.length, nullptr);

	if (cache_inserted) {
		// Get buffer pointer from cache
		char *cache_buf = nullptr;
		int cache_len = r.length;
		m_cache.get(loc, [&](char const *data) {
			cache_buf = const_cast<char *>(data);
		});

		if (cache_buf) {
			// Write-through: immediately write to disk using cache buffer
			boost::asio::post(write_thread_pool_,
				[=, this, handler = std::move(handler)]() {
					libtorrent::storage_error error;

					// Write to disk (using cache's buffer)
					auto const start_time = libtorrent::clock_type::now();
					storages_[storage]->write(cache_buf, r.piece, r.start, cache_len, error);
					auto const write_time = libtorrent::total_microseconds(libtorrent::clock_type::now() - start_time);

					// Update counters
					m_stats_counters.inc_stats_counter(libtorrent::counters::num_blocks_written);
					m_stats_counters.inc_stats_counter(libtorrent::counters::num_write_ops);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_write_time, write_time);
					m_stats_counters.inc_stats_counter(libtorrent::counters::disk_job_time, write_time);

					// Mark cache entry as clean (write completed)
					m_cache.mark_clean(loc);

					// Call handler
					post(ioc_, [=, h = std::move(handler)] {
						h(error);
					});
				});

			return false;  // No buffer pool, so never exceeded
		}
		// Cache get failed - fall through to sync write
	}
	// Cache insert failed - fall through to sync write

	// Fallback to sync write (cache unavailable)
	spdlog::debug("[async_write] Sync write fallback - cache unavailable (storage={}, piece={}, offset={})",
		static_cast<int>(storage), static_cast<int>(r.piece), r.start);

	// sync
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
	return false;  // No buffer pool used, so never exceeded
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


}  // namespace ezio
