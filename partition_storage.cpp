#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#include <boost/assert.hpp>
#include <spdlog/spdlog.h>

#include "partition_storage.hpp"

namespace ezio
{

partition_storage::partition_storage(const std::string &path, libtorrent::file_storage const &fs) :
	m_fs(fs)
{
	m_fd = open(path.c_str(), O_RDWR);
	if (m_fd < 0) {
		spdlog::critical("failed to open ({}) = {}", path, strerror(m_fd));
		exit(1);
	}
}

partition_storage::~partition_storage()
{
	int ec = close(m_fd);
	if (ec) {
		spdlog::error("close: {}", strerror(ec));
	}
}

int partition_storage::piece_size(libtorrent::piece_index_t const piece)
{
	return m_fs.piece_size(piece);
}

int partition_storage::read(char *buffer, libtorrent::piece_index_t const piece, int const offset,
	int const length, libtorrent::storage_error &error)
{
	BOOST_ASSERT(buffer != nullptr);

	auto file_slices = m_fs.map_block(piece, offset, length);
	int ret = 0;

	for (const auto &file_slice : file_slices) {
		const auto &file_index = file_slice.file_index;

		int64_t partition_offset = 0;

		// to find partition_offset from file name.
		std::string file_name(m_fs.file_name(file_index));
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

		pread(m_fd, buffer, file_slice.size, partition_offset);
		ret += file_slice.size;
		buffer += file_slice.size;
	}
	return ret;
}

void partition_storage::write(char *buffer, libtorrent::piece_index_t const piece, int const offset,
	int const length, libtorrent::storage_error &error)
{
	BOOST_ASSERT(buffer != nullptr);

	auto file_slices = m_fs.map_block(piece, offset, length);

	for (const auto &file_slice : file_slices) {
		const auto &file_index = file_slice.file_index;

		int64_t partition_offset = 0;

		// to find partition_offset from file name.
		std::string file_name(m_fs.file_name(file_index));
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

		pwrite(m_fd, buffer, file_slice.size, partition_offset);
		buffer += file_slice.size;
	}
}

}  // namespace ezio
