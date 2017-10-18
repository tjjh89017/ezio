#include <errno.h>
#include <string>

#include <libtorrent/file_storage.hpp>

#include "error_code.hpp"

namespace ezio {
	namespace storage {
		const int MAX_FILENAME_LENGTH = 33;

		struct raw_storage : libtorrent::storage_interface {
			libtorrent::file_storage m_files;
			int target_fd;
			const std::string target_partition_path;

			raw_storage(libtorrent::file_storage const& fs, const std::string tp) :
				m_files(fs), target_partition_path(tp) {}

			void initialize(libtorrent::storage_error& err) throw()
			{
				// Open disk fd
				this->target_fd = open(target_partition_path.c_str(), O_RDWR | O_CREAT);
				if (this->target_fd < 0) { // Failed
					boost::system::error_code ecode = errors::make_error_code(errors::failed_to_open_disk);
					err.ec = ecode;
					err.operation = libtorrent::storage_error::open;
				}
			}

			// assume no resume
			bool has_any_file(libtorrent::storage_error& err) { return false; }

			int readv(libtorrent::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, libtorrent::storage_error& err)
			{
				int index = 0;
				int i = 0;
				int ret = 0;
				unsigned long long device_offset = 0;
				unsigned long long fd_offset = 0; // A fd' point we read data from fd from
				unsigned long long cur_offset = 0; // A pieces' point we have to write data until
				unsigned long long remain_len = 0;
				unsigned long long piece_sum = 0;
				unsigned long long data_len = 0;
				char *data_buf, *data_ptr = NULL;
				char filename[MAX_FILENAME_LENGTH]; // Should be the max length of file name

				// Get file name from offset
				index = m_files.file_index_at_offset(piece * std::uint64_t(m_files.piece_length()) + offset);
				memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
				filename[m_files.file_name_len(index)] = 0;
				sscanf(filename, "%llx", &device_offset);

				// Caculate total piece size of previous files
				for (i = 0; i < index; i++)
					piece_sum += m_files.file_size(i);

				// Caculate the length of all bufs
				for (i = 0; i < num_bufs; i++)
					data_len += bufs[i].iov_len;
				data_buf = (char *)malloc(data_len);

				// Read fd to data_buf
				cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
				fd_offset = device_offset + offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum;
				remain_len = m_files.file_size(index) - (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
				data_ptr = data_buf;
				while (data_len > 0) {
					if (data_len > remain_len) {
						ret += pread(this->target_fd, data_ptr, remain_len, fd_offset);
						data_len -= remain_len;
						data_ptr += remain_len;
						cur_offset += remain_len;
						index = m_files.file_index_at_offset(cur_offset);
						memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
						filename[m_files.file_name_len(index)] = 0;
						sscanf(filename,"%llx", &fd_offset);
						remain_len = m_files.file_size(index);
					} else {
						ret += pread(this->target_fd, data_ptr, data_len, fd_offset);
						data_len -= data_len;
					}
				}

				// Copy data_buf to bufs
				data_ptr = data_buf;
				for (i = 0; i < num_bufs; i++) {
					memcpy(bufs[i].iov_base, data_ptr, bufs[i].iov_len);
					data_ptr += bufs[i].iov_len;
				}

				free(data_buf);
				return ret;
			}

			int writev(libtorrent::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, libtorrent::storage_error& ec)
			{
				int index = 0;
				int i = 0;
				int ret = 0;
				unsigned long long device_offset = 0;
				unsigned long long fd_offset = 0; // A fd' point we write data to fd from
				unsigned long long cur_offset = 0; // A pieces' point we have to read data until
				unsigned long long remain_len = 0;
				unsigned long long piece_sum = 0;
				unsigned long long data_len = 0;
				char *data_buf = NULL, *data_ptr = NULL;
				char filename[MAX_FILENAME_LENGTH]; // Should be the max length of file name

				// Get file name from offset
				index = m_files.file_index_at_offset(piece * std::uint64_t(m_files.piece_length()) + offset);
				memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
				filename[m_files.file_name_len(index)] = 0;
				sscanf(filename, "%llx", &device_offset);

				// Caculate total piece size of previous files
				for (i = 0; i < index; i++)
					piece_sum += m_files.file_size(i);

				// Caculate the length of all bufs
				for (i = 0; i < num_bufs; i++)
					data_len += bufs[i].iov_len;

				// Merge all bufs into data_buf
				data_buf = (char *) malloc(data_len);
				data_ptr = data_buf;
				for (i = 0; i < num_bufs; i++) {
					memcpy(data_ptr, bufs[i].iov_base, bufs[i].iov_len);
					data_ptr += bufs[i].iov_len;
				}

				// Write data_buf to fd
				cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
				fd_offset = device_offset + offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum;
				remain_len = m_files.file_size(index) - (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
				data_ptr = data_buf;
				while (data_len > 0) {
					if (data_len > remain_len) {
						ret += pwrite(this->target_fd, data_ptr, remain_len, fd_offset);
						data_len -= remain_len;
						data_ptr += remain_len;
						cur_offset += remain_len;
						index = m_files.file_index_at_offset( cur_offset );
						memcpy(filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
						filename[m_files.file_name_len(index)] = 0;
						sscanf(filename, "%llx", &fd_offset);
						remain_len = m_files.file_size(index);
					} else {
						ret += pwrite(this->target_fd, data_ptr, data_len, fd_offset);
						data_len -= data_len;
					}
				}
				free(data_buf);
				return ret;
			}

			// Not need
			void rename_file(int index, std::string const& new_filename, libtorrent::storage_error& ec)
			{ assert(false); return; }

			int move_storage(std::string const& save_path, int flags, libtorrent::storage_error& ec) { return 0; }
			bool verify_resume_data(libtorrent::bdecode_node const& rd
							, std::vector<std::string> const* links
							, libtorrent::storage_error& error) { return false; }
			void write_resume_data(libtorrent::entry& rd, libtorrent::storage_error& ec) const {}
			void set_file_priority(std::vector<boost::uint8_t> const& prio, libtorrent::storage_error& ec) {}
			void release_files(libtorrent::storage_error& ec) {}
			void delete_files(int i, libtorrent::storage_error& ec) {}

			bool tick() { return false; };
		}; // struct raw_storage
	}; // namespace storage
}; // namespace ezio
