#include "raw_storage.hpp"
#include <exception>

lt::storage_interface* raw_storage::raw_storage_constructor(lt::storage_params const& params)
{
    return nullptr;
}

raw_storage::raw_storage(lt::file_storage const& fs, const std::string tp) : m_files(fs), target_partition(tp)
{
	this->fd = open(target_partition.c_str(), O_RDWR | O_CREAT);
	if(this->fd < 0){
		// Failed handle
		std::cerr << "Failed to open " << target_partition << std::endl;

		// TODO exit
	}
	return;
}

raw_storage::~raw_storage()
{
	close(this->fd);
}

void raw_storage::initialize(lt::storage_error& se) {}

// assume no resume
bool raw_storage::has_any_file(lt::storage_error& ec) { return false; }

int raw_storage::readv(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
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
	char filename[33]; // Should be the max length of file name

	// Get file name from offset
	index = m_files.file_index_at_offset( piece * std::uint64_t(m_files.piece_length()) + offset);
	memcpy( filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
	filename[m_files.file_name_len(index)] = 0;
	sscanf(filename,"%llx", &device_offset);

	// Caculate total piece size of previous files
	for( i = 0 ; i < index; i++ )
		piece_sum += m_files.file_size(i);

	// Caculate the length of all bufs
	for( i = 0 ; i < num_bufs ; i ++){
		data_len += bufs[i].iov_len;
	}
	data_buf = (char *)malloc(data_len);

	// Read fd to data_buf
	cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
	fd_offset = device_offset + offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum;
	remain_len = m_files.file_size(index) - (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
	data_ptr = data_buf;
	while(data_len > 0){
		if( data_len > remain_len ){
			ret += pread(this->fd, data_ptr, remain_len, fd_offset);
			data_len -= remain_len;
			data_ptr += remain_len;
			cur_offset += remain_len;
			index = m_files.file_index_at_offset( cur_offset );
			memcpy( filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
			filename[m_files.file_name_len(index)] = 0;
			sscanf(filename,"%llx", &fd_offset);
			remain_len = m_files.file_size(index);
		}
		else{
			ret += pread(this->fd, data_ptr, data_len, fd_offset);
			data_len -= data_len;
		}
	}

	// Copy data_buf to bufs
	data_ptr = data_buf;
	for( i = 0 ; i < num_bufs ; i ++){
		memcpy(bufs[i].iov_base, data_ptr, bufs[i].iov_len);
		data_ptr += bufs[i].iov_len;
	}

	free(data_buf);
	return ret;
}

namespace ezioTest 
{

class TestError : public std::exception
{
    int m_errno;
public:
    TestError(int _errno = 0) :
        m_errno(_errno)
    {}
};


class FS
{
    int ret = 0;
public:
   static ssize_t write(int fildes, const void *buf, size_t nbyte, off_t offset)
   {
        int ret = pwrite(fildes, buf, nbyte, offset);
        if(-1 == ret) {
            throw TestError(ret);
        }

        return ret;
   }
};

}

int raw_storage::writev(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
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
	char filename[33]; // Should be the max length of file name

	// Get file name from offset
	index = m_files.file_index_at_offset( piece * std::uint64_t(m_files.piece_length()) + offset);
	memcpy( filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
	filename[m_files.file_name_len(index)] = 0;
	sscanf(filename,"%llx", &device_offset);

	// Caculate total piece size of previous files
	for( i = 0 ; i < index; i++ )
		piece_sum += m_files.file_size(i);

	// Caculate the length of all bufs
	for( i = 0 ; i < num_bufs ; i ++){
		data_len += bufs[i].iov_len;
	}

	// Merge all bufs into data_buf
	data_buf = (char *)malloc(data_len);
	data_ptr = data_buf;
	for( i = 0 ; i < num_bufs ; i ++){
		memcpy(data_ptr, bufs[i].iov_base, bufs[i].iov_len);
		data_ptr += bufs[i].iov_len;
	}

	// Write data_buf to fd
	cur_offset = piece * std::uint64_t(m_files.piece_length()) + offset;
	fd_offset = device_offset + offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum;
	remain_len = m_files.file_size(index) - (offset + piece * std::uint64_t(m_files.piece_length()) - piece_sum);
	data_ptr = data_buf;
	while(data_len > 0){
		if( data_len > remain_len ){
			ret += pwrite(this->fd, data_ptr, remain_len, fd_offset);
			data_len -= remain_len;
			data_ptr += remain_len;
			cur_offset += remain_len;
			index = m_files.file_index_at_offset( cur_offset );
			memcpy( filename, m_files.file_name_ptr(index), m_files.file_name_len(index));
			filename[m_files.file_name_len(index)] = 0;
			sscanf(filename,"%llx", &fd_offset);
			remain_len = m_files.file_size(index);
		}
		else{
			ret += ezioTest::FS::write(this->fd, data_ptr, data_len, fd_offset);
			data_len -= data_len;
		}
	}
	free(data_buf);
	return ret;
}

// Not need
void raw_storage::rename_file(int index, std::string const& new_filename, lt::storage_error& ec)
{ assert(false); return ; }

int raw_storage::move_storage(std::string const& save_path, int flags, lt::storage_error& ec) { return 0; }
bool raw_storage::verify_resume_data(lt::bdecode_node const& rd
				, std::vector<std::string> const* links
				, lt::storage_error& error) { return false; }
void raw_storage::write_resume_data(lt::entry& rd, lt::storage_error& ec) const { return ; }
void raw_storage::set_file_priority(std::vector<boost::uint8_t> const& prio, lt::storage_error& ec) {return ;}
void raw_storage::release_files(lt::storage_error& ec) { return ; }
void raw_storage::delete_files(int i, lt::storage_error& ec) { return ; }

bool raw_storage::tick () { return false; };
