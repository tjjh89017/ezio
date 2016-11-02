#include <iostream>
#include <thread>
#include <chrono>

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/storage.hpp>
#include <libtorrent/io.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>

#include <boost/progress.hpp>

namespace lt = libtorrent;

struct temp_storage : lt::storage_interface {
	temp_storage(lt::file_storage const& fs, const std::string tp) : m_files(fs), target_partition(tp) {}
	// Open disk fd
	void initialize(lt::storage_error& se)
	{
		this->fd = open(target_partition.c_str(), O_RDWR | O_CREAT);
		if(this->fd < 0){
			// Failed handle
			std::cerr << "Failed to open " << target_partition << std::endl;

			// TODO exit
		}
		return;
	}

	// assume no resume
	bool has_any_file(lt::storage_error& ec) { return false; }

	int readv(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
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

	int writev(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
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
				ret += pwrite(this->fd, data_ptr, data_len, fd_offset);
				data_len -= data_len;
			}
		}
		free(data_buf);
		return ret;
	}

	// Not need
	void rename_file(int index, std::string const& new_filename, lt::storage_error& ec)
	{ assert(false); return ; }

	int move_storage(std::string const& save_path, int flags, lt::storage_error& ec) { return 0; }
	bool verify_resume_data(lt::bdecode_node const& rd
					, std::vector<std::string> const* links
					, lt::storage_error& error) { return false; }
	void write_resume_data(lt::entry& rd, lt::storage_error& ec) const { return ; }
	void set_file_priority(std::vector<boost::uint8_t> const& prio, lt::storage_error& ec) {return ;}
	void release_files(lt::storage_error& ec) { return ; }
	void delete_files(int i, lt::storage_error& ec) { return ; }

	bool tick () { return false; };


	lt::file_storage m_files;
	int fd;
	const std::string target_partition;
};


lt::storage_interface* temp_storage_constructor(lt::storage_params const& params)
{
	return new temp_storage(*params.files, params.path);
}

int main(int argc, char const* argv[])
{
	if (argc != 3) {
		std::cerr << "usage: " << argv[0] << " <magnet-url/torrent-file> <target-partition-path>" << std::endl;
		return 1;
	}
	lt::session ses;
	lt::error_code ec;
	lt::settings_pack set;

	// setting
	// we don't need DHT
	set.set_bool(lt::settings_pack::enable_dht, false);
	ses.apply_settings(set);

	lt::add_torrent_params atp;

	// magnet or torrent
	// TODO find a better way
	std::string bt_info = argv[1];
	if(bt_info.substr(bt_info.length() - 8, 8) == ".torrent"){
		atp.ti = boost::make_shared<lt::torrent_info>(bt_info, boost::ref(ec), 0);
	}
	else{
		atp.url = argv[1];
	}
	atp.save_path = argv[2];
	atp.storage = temp_storage_constructor;

	lt::torrent_handle handle = ses.add_torrent(atp);
	handle.set_max_uploads(4);
	//boost::progress_display show_progress(100, std::cout);
	unsigned long last_progess = 0, progress = 0;
	lt::torrent_status status;

	for(;;){
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		status = handle.status();
		// progress
		last_progess = progress;
		progress = status.progress * 100;
		//show_progress += progress - last_progess;
		std::cout << "\r"
			<< "[T: " << progress << "%] "
			<< "[D: " << (float)status.download_payload_rate / 1024 / 1024 << "MB/s] "
			<< "[U: " << (float)status.upload_payload_rate / 1024 / 1024 << "MB/s] "
			<< std::flush;

		for (lt::alert const* a : alerts) {
			// std::cout << a->message() << std::endl;
			// if we receive the finished alert or an error, we're done
			if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
				goto done;
			}
			if (status.is_finished) {
				goto done;
			}
			if (lt::alert_cast<lt::torrent_error_alert>(a)) {
				std::cerr << "Error" << std::endl;
				return 1;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	done:
	std::cout << std::endl;


	// Start high performance seed
	lt::high_performance_seed(set);
	ses.apply_settings(set);
	std::cout << "Start seeding" << std::endl;

	// seed until idle 15mins
	int timeout = 15 * 60;

	// seed until seed rate 300%
	boost::int64_t seeding_rate_limit = 3;
	boost::int64_t total_size = handle.torrent_file()->total_size();

	for (;;) {
		status = handle.status();
		int utime = status.time_since_upload;
		int dtime = status.time_since_download;
		boost::int64_t total_payload_upload = status.total_payload_upload;

		std::cout << "\r"
			<< "[T: " << progress << "%] "
			<< "[D: " << (float)status.download_payload_rate / 1024 / 1024 << "MB/s] "
			<< "[U: " << (float)status.upload_payload_rate / 1024 / 1024 << "MB/s] "
			<< std::flush;

		if(utime == -1 && timeout < dtime){
			break;
		}
		else if(timeout < utime){
			// idle 15mins
			break;
		}
		else if(seeding_rate_limit < (total_payload_upload / total_size)){
			// seeding 300%
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	std::cout << std::endl;
	std::cout << "done, shutting down" << std::endl;

	return 0;
}
