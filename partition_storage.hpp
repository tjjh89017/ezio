#ifndef __PARTITION_STORAGE_HPP__
#define __PARTITION_STORAGE_HPP__

#include <string>
#include <libtorrent/libtorrent.hpp>

namespace ezio
{

class partition_storage
{
private:
	int m_fd{0};
	void *m_mapping_addr{nullptr};
	size_t m_mapping_len{0};

	libtorrent::file_storage const &m_fs;

public:
	partition_storage(const std::string &path, libtorrent::file_storage const &fs);
	~partition_storage();

	int piece_size(libtorrent::piece_index_t const piece);

	int read(char *buffer, libtorrent::piece_index_t const piece, int const offset,
		int const length, libtorrent::storage_error &error);

	void write(char *buffer, libtorrent::piece_index_t const piece, int const offset,
		int const length, libtorrent::storage_error &error);
};

}  // namespace ezio

#endif
