#include <string>
#include <sys/mman.h>

#include <boost/assert.hpp>
#include <spdlog/spdlog.h>
#include <libtorrent/hasher.hpp>

#include "raw_disk_io.hpp"
#include "buffer_pool.hpp"

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

		// calc total size for calling mmap() later.
		size_t length = 0;
		for (const auto &file_index : fs.file_range()) {
			std::string file_name(fs.file_name(file_index));
			int64_t file_size = fs.file_size(file_index);

			try {
				int64_t file_offset = std::stoll(file_name, 0, 16);
				int64_t end = file_offset + file_size;
				if (length < end) {
					length = end;
				}
			} catch (const std::exception &e) {
				SPDLOG_CRITICAL("failed to parse file_name({}) at ({}): {}",
					file_name, file_index, e.what());
				exit(1);
			}
		}

		mapping_len_ = length;
		mapping_addr_ = mmap(nullptr, length, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd_, 0);
		if (mapping_addr_ == MAP_FAILED) {
			SPDLOG_CRITICAL("failed to mmap: {}", strerror(errno));
			exit(1);
		}
	}

	~partition_storage()
	{
		int ec = munmap(mapping_addr_, mapping_len_);
		if (ec) {
			SPDLOG_ERROR("munmap: {}", strerror(ec));
		}

		ec = close(fd_);
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
		BOOST_ASSERT(mapping_addr_ != nullptr);

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

			memcpy(buffer, mapping_addr_ + partition_offset, file_slice.size);
			ret += file_slice.size;
			buffer += file_slice.size;
		}
		return ret;
	}

	void write(char *buffer, libtorrent::piece_index_t const piece, int const offset,
		int const length, libtorrent::storage_error &error)
	{
		BOOST_ASSERT(buffer != nullptr);
		BOOST_ASSERT(mapping_addr_ != nullptr);

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

			memcpy(mapping_addr_ + partition_offset, buffer, file_slice.size);
			buffer += file_slice.size;
		}
	}
};

std::unique_ptr<libtorrent::disk_interface> raw_disk_io_constructor(libtorrent::io_context &ioc,
	libtorrent::settings_interface const &s,
	libtorrent::counters &c)
{
	return std::make_unique<raw_disk_io>(ioc);
}

raw_disk_io::raw_disk_io(libtorrent::io_context &ioc) :
	ioc_(ioc),
	read_buffer_pool_(ioc),
	write_buffer_pool_(ioc),
	thread_pool_(8),
	hash_thread_pool_(8)
{
}

raw_disk_io::~raw_disk_io()
{
	thread_pool_.join();
}

libtorrent::storage_holder raw_disk_io::new_torrent(libtorrent::storage_params const &p,
	std::shared_ptr<void> const &)
{
	const std::string &target_partition = p.path;

	int idx = storages_.size();
	auto storage = std::make_unique<partition_storage>(target_partition, p.files);
	storages_.emplace(idx, std::move(storage));

	if (idx > 0) {
		SPDLOG_WARN("new_torrent current idx => {}, should be 0", idx);
	}

	return libtorrent::storage_holder(idx, *this);
}

void raw_disk_io::remove_torrent(libtorrent::storage_index_t idx)
{
	SPDLOG_WARN("unsupported operation: remove_torrent({})", idx);
}

void raw_disk_io::async_read(
	libtorrent::storage_index_t idx, libtorrent::peer_request const &r,
	std::function<void(libtorrent::disk_buffer_holder, libtorrent::storage_error const &)> handler,
	libtorrent::disk_job_flags_t flags)
{
	BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

	libtorrent::storage_error error;
	char *buf = read_buffer_pool_.allocate_buffer();

	if (!buf) {
		error.ec = libtorrent::errors::no_memory;
		error.operation = libtorrent::operation_t::alloc_cache_piece;
		post(ioc_, [=, h = std::move(handler)] {
			h(libtorrent::disk_buffer_holder(read_buffer_pool_, nullptr, 0), error);
		});
		return;
	}
	
	boost::asio::post(thread_pool_,
		[=, this, handler = std::move(handler)]()
		{
			libtorrent::storage_error error;
			storages_[idx]->read(buf, r.piece, r.start, r.length, error);
			auto buffer = libtorrent::disk_buffer_holder(read_buffer_pool_, buf, r.length);

			post(ioc_, [h = std::move(handler), b = std::move(buffer), error]() mutable {
				h(std::move(b), error);
			});
		}
	);
}

bool raw_disk_io::async_write(libtorrent::storage_index_t storage, libtorrent::peer_request const &r,
	char const *buf, std::shared_ptr<libtorrent::disk_observer> o,
	std::function<void(libtorrent::storage_error const &)> handler,
	libtorrent::disk_job_flags_t flags)
{
	BOOST_ASSERT(DEFAULT_BLOCK_SIZE >= r.length);

	bool exceeded = false;
	char *buffer = write_buffer_pool_.allocate_buffer(exceeded, o);

	if (buffer) {
		// async
		libtorrent::disk_buffer_holder buffer_holder(write_buffer_pool_, buffer, DEFAULT_BLOCK_SIZE);
		memcpy(buffer_holder.data(), buf, r.length);
		boost::asio::post(thread_pool_,
			[=, this, handler = std::move(handler), buffer_holder = std::move(buffer_holder)]()
			{
				libtorrent::storage_error error;
				storages_[storage]->write(buffer_holder.data(), r.piece, r.start, r.length, error);

				post(ioc_, [=, h = std::move(handler)] {
					h(error);
				});
			}	
		);
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
		[=, this, handler = std::move(handler), buffer = std::move(buffer)]()
		{
			libtorrent::storage_error error;
			libtorrent::hasher ph;
			partition_storage *st = storages_[storage].get(); 

			int const piece_size = st->piece_size(piece);
			int const blocks_in_piece = (piece_size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

			int offset = 0;
			int const blocks_to_read = blocks_in_piece;
			for (int i = 0; i < blocks_to_read; i++) {
				auto const len = std::min(DEFAULT_BLOCK_SIZE, piece_size - offset);
				int const ret = st->read(buf, piece, offset, len, error);
				if (ret <= 0) {
					break;
				}
				offset += DEFAULT_BLOCK_SIZE;
				ph.update(buf, ret);
			}

			libtorrent::sha1_hash const hash = ph.final();

			post(ioc_, [=, h = std::move(handler)]{ h(piece, hash, error); });
		}
	);
	
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
