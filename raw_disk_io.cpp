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
			SPDLOG_CRITICAL("failed to open ({}) = {}", path, strerror(fd_));
			exit(1);
		}
	}

	~partition_storage()
	{
		int ec = close(fd_);
		if (ec) {
			SPDLOG_ERROR("close: {}", strerror(ec));
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
				SPDLOG_CRITICAL("failed to parse file_name({}) at ({}): {}",
					file_name, file_index, e.what());
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
				SPDLOG_CRITICAL("failed to parse file_name({}) at ({}): {}",
					file_name, file_index, e.what());
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
	return std::make_unique<raw_disk_io>(ioc, 16);
}

raw_disk_io::raw_disk_io(libtorrent::io_context &ioc, int cache) :
	ioc_(ioc),
	read_buffer_pool_(ioc),
	write_buffer_pool_(ioc),
	read_thread_pool_(8),
	write_thread_pool_(8),
	hash_thread_pool_(8),
	cache_(cache * 64)
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
		SPDLOG_WARN("new_torrent current idx => {}, should be 0", idx);
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

	char *buf = read_buffer_pool_.allocate_buffer();
	libtorrent::disk_buffer_holder buffer(read_buffer_pool_, buf, DEFAULT_BLOCK_SIZE);
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
		// unaligned
		torrent_location const loc1{idx, r.piece, block_offset};
		torrent_location const loc2{idx, r.piece, block_offset + DEFAULT_BLOCK_SIZE};
		std::ptrdiff_t const len1 = DEFAULT_BLOCK_SIZE - read_offset;

		BOOST_ASSERT(r.length > len1);

		int ret = store_buffer_.get2(loc1, loc2, [&](char const *buf1, char const *buf2) {
			if (buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
			}
			if (buf2) {
				std::memcpy(buf + len1, buf2, std::size_t(r.length - len1));
			}
			return (buf1 ? 2 : 0) | (buf2 ? 1 : 0);
		});

		if ((ret & 2) == 0) {
			bool r1 = cache_.get(loc1, [&](char const *buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(len1));
			});
			ret |= 2;
		}

		if ((ret & 1) == 0) {
			bool r2 = cache_.get(loc2, [&](char const *buf1) {
				std::memcpy(buf + len1, buf1, std::size_t(r.length - len1));
			});
		}

		if (ret == 3) {
			// success get whole piece
			// return immediately
			handler(std::move(buffer), error);
			return;
		}

		if (ret != 0) {
			// partial
			boost::asio::post(read_thread_pool_,
				[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
					libtorrent::storage_error error;
					auto offset = (ret == 1) ? r.start : block_offset + DEFAULT_BLOCK_SIZE;
					auto len = (ret == 1) ? len1 : r.length - len1;
					auto buf_offset = (ret == 1) ? 0 : len1;
					char *tmp = (char *)malloc(DEFAULT_BLOCK_SIZE);
					storages_[idx]->read(tmp, r.piece, offset, DEFAULT_BLOCK_SIZE, error);
					cache_.insert({idx, r.piece, offset}, tmp);
					SPDLOG_INFO("Put Cache {} {}", r.piece, offset);
					std::memcpy(buf + buf_offset, tmp + offset, len);
					free(tmp);

					post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
						h(std::move(b), error);
					});
				});
			return;
		}

		// if we cannot find any block, post it as normal job
	} else {
		// aligned block
		if (store_buffer_.get({idx, r.piece, block_offset}, [&](char const *buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(r.length));
			})) {
			handler(std::move(buffer), error);
			return;
		}

		if (cache_.get({idx, r.piece, block_offset}, [&](char const *buf1) {
				std::memcpy(buf, buf1 + read_offset, std::size_t(r.length));
			})) {
			handler(std::move(buffer), error);
			return;
		}
	}

	boost::asio::post(read_thread_pool_,
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]() mutable {
			libtorrent::storage_error error;
			storages_[idx]->read(buf, r.piece, r.start, r.length, error);

			cache_.insert({idx, r.piece, r.start}, buf);

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
	libtorrent::disk_buffer_holder buffer(write_buffer_pool_, write_buffer_pool_.allocate_buffer(exceeded, o), DEFAULT_BLOCK_SIZE);

	cache_.insert({storage, r.piece, r.start}, buf);
	if (buffer) {
		// async
		memcpy(buffer.data(), buf, r.length);
		store_buffer_.insert({storage, r.piece, r.start}, buffer.data());

		libtorrent::peer_request r2(r);
		boost::asio::post(write_thread_pool_,
			[=, this, handler = std::move(handler), buffer = std::move(buffer)]() {
				libtorrent::storage_error error;
				storages_[storage]->write(buffer.data(), r.piece, r.start, r.length, error);

				store_buffer_.erase({storage, r.piece, r.start});

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
	char *buf = read_buffer_pool_.allocate_buffer();

	if (!buf) {
		error.ec = libtorrent::errors::no_memory;
		error.operation = libtorrent::operation_t::alloc_cache_piece;
		post(ioc_, [=, h = std::move(handler)] {
			h(piece, libtorrent::sha1_hash{}, error);
		});
		return;
	}

	auto buffer = libtorrent::disk_buffer_holder(read_buffer_pool_, buf, DEFAULT_BLOCK_SIZE);

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
				bool hit = store_buffer_.get({storage, piece, offset}, [&](char const *buf1) {
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
}

}  // namespace ezio
