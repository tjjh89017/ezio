#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#define RAM_2G (2UL * 1024 * 1024 * 1024)
#endif

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

int timeout_ezio = 15; // Default timeout (min)
int seed_limit_ezio = 3; // Default seeding ratio limit
int max_upload_ezio = 4;
int max_contact_tracker_times = 30; // Max error times for scrape tracker

struct raw_storage : lt::storage_interface {
	raw_storage(lt::file_storage const& fs, const std::string tp) : m_files(fs), target_partition(tp) {}
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


lt::storage_interface* raw_storage_constructor(lt::storage_params const& params)
{
	return new raw_storage(*params.files, params.path);
}

void usage()
{
      std::cerr << "Usage: ezio [OPTIONS] <magnet-url/torrent-file> <target-partition-path>\n"
                << "OPTIONS:\n"
                << "	-e N: assign seeding ratio limit as N. Default value is " << seed_limit_ezio <<"\n"
                << "	-k N: assign maxminum failure number to contact tracker as N. Default value is " << max_contact_tracker_times<< "\n"
                << "	-m N: assign maxminum upload number as N. Default value is " << max_upload_ezio <<"\n"
                << "	-s: enable sequential download\n"
                << "	-t N: assign timeout as N min(s). Default value " << timeout_ezio
      		<< std::endl;
}

int main(int argc, char ** argv)
{
	lt::add_torrent_params atp;
	int opt;
	int opt_n = 0;
	int seq_flag = 0;

	opterr = 0;
	while (( opt = getopt (argc, argv, "e:m:st:")) != -1)
	  switch (opt)
	    {
	    case 'e':
	      seed_limit_ezio = atoi(optarg);
	      ++opt_n;
	      ++opt_n;
	      break;
	    case 'm':
	      max_upload_ezio = atoi(optarg);
	      ++opt_n;
	      ++opt_n;
	      break;
	    case 's':
	      seq_flag = 1;
	      ++opt_n;
	      break;
	    case 't':
	      timeout_ezio = atoi(optarg);
	      ++opt_n;
	      ++opt_n;
	      break;
	    case '?':
	      usage();
	      return 1;
	    default:
	      usage();
	      exit(EXIT_FAILURE);
	}

	if (argc - opt_n != 3) {
		usage();
		return 1;
	}
	std::string bt_info = argv[optind];
	++optind;;
	atp.save_path = argv[optind];

	if (seq_flag) {
	  std::cout << "//NOTE// Sequential download is enabled!" << std::endl;
	}

	lt::session ses;
	lt::error_code ec;
	lt::settings_pack set;

	// setting
	// we don't need DHT
	set.set_bool(lt::settings_pack::enable_dht, false);
#ifdef __linux__
	// Determine Physical Ram Size
	// if more than 2GB, set cache to half of Ram
	struct sysinfo info;
	if(sysinfo(&info) == 0) {
		unsigned long totalram = info.totalram * info.mem_unit;
		if(totalram > RAM_2G) {
			// unit: blocks per 16KiB
			int size = (int)(totalram / 16 / 1024 / 2);
			set.set_int(lt::settings_pack::cache_size, size);
		}
	}
#endif
	ses.apply_settings(set);

	// magnet or torrent
	// TODO find a better way
	if(bt_info.substr(bt_info.length() - 8, 8) == ".torrent"){
		atp.ti = boost::make_shared<lt::torrent_info>(bt_info, boost::ref(ec), 0);
	}
	else{
		atp.url = bt_info;
	}
	atp.storage = raw_storage_constructor;

	lt::torrent_handle handle = ses.add_torrent(atp);
	handle.set_max_uploads(max_upload_ezio);
	handle.set_sequential_download(seq_flag);
	//boost::progress_display show_progress(100, std::cout);
	unsigned long last_progess = 0, progress = 0;
	lt::torrent_status status;

	std::cout << "Start downloading" << std::endl;

	for(;;){
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		status = handle.status();
		// progress
		last_progess = progress;
		progress = status.progress * 100;
		//show_progress += progress - last_progess;
		std::cout << "\r"
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)status.download_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[DT: " << (int)status.active_time  << " secs] "
			<< "[U: " << std::setprecision(2) << (float)status.upload_payload_rate / 1024 / 1024 /1024 *60 << " GB/min] "
			<< "[UT: " << (int)status.seeding_time  << " secs] "
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
	std::cout << "Start high-performance seeding" << std::endl;

	// seed until idle (secs)
	int timeout = timeout_ezio * 60;

	// seed until seed rate
	boost::int64_t seeding_rate_limit = seed_limit_ezio;
	boost::int64_t total_size = handle.torrent_file()->total_size();

	int fail_contact_tracker = 0;
	for (;;) {
		status = handle.status();
		int utime = status.time_since_upload;
		int dtime = status.time_since_download;
		boost::int64_t total_payload_upload = status.total_payload_upload;
		// ses.set_alert_mask(lt::alert::tracker_notification | lt::alert::error_notification);
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		std::cout << "\r"
			/*
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)status.download_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[T: " << (int)status.active_time  << " secs] "
			*/
			<< "[U: " << std::setprecision(2) << (float)status.upload_payload_rate / 1024 / 1024 /1024 * 60 << " MB/min] "
			<< "[T: " << (int)status.seeding_time  << " secs] "
			<< std::flush;

		if(utime == -1 && timeout < dtime){
			break;
		}
		else if(timeout < utime){
			break;
		}
		else if(seeding_rate_limit < (total_payload_upload / total_size)){
			break;
		}

		handle.scrape_tracker();
		for (lt::alert const* a : alerts) {
			if (lt::alert_cast<lt::scrape_failed_alert>(a)) {
				++fail_contact_tracker;;
			}
		}

		if(fail_contact_tracker > max_contact_tracker_times){
	                std::cout << "\nTracker is gone! Finish seeding!" << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	std::cout << "\nDone, shutting down" << std::endl;

	return 0;
}
