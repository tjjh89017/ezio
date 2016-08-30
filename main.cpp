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

#include <boost/progress.hpp>

namespace lt = libtorrent;

struct temp_storage : lt::storage_interface {
	temp_storage(lt::file_storage const& fs, const std::string tp) : m_files(fs), target_partition(tp) {}
	// Open disk fd
	void initialize(lt::storage_error& se)
	{
		std::cerr << "initialize" << std::endl;
		std::cerr << "sizeof m_files" << std::endl;
		this->fd = open(target_partition.c_str(), O_RDWR | O_CREAT);
		std::cerr << "writing to: " << target_partition << '\n';

		return;
	}

	// assume no resume
	bool has_any_file(lt::storage_error& ec) { return false; }

	// 
	int readv(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
	{
		/*
		std::cerr << "readv: " << std::endl;
		std::cerr << num_bufs << std::endl;
		std::cerr << piece << std::endl;
		std::cerr << offset << std::endl;

		for(int i = 0; i < num_bufs; i++){
			std::cerr << bufs[i].iov_len << std::endl;
		}
		*/

		// std::map<int, std::vector<char> >::const_iterator i = m_file_data.find(piece);
		// if (i == m_file_data.end()) return 0;
		// int available = i->second.size() - offset;
		// if (available <= 0) return 0;
		// if (available > size) available = size;
		// memcpy(buf, &i->second[offset], available);
		// return available;
		return preadv(this->fd, bufs, num_bufs, piece * std::uint64_t(m_files.piece_length()) + offset);
	}
	int writev(lt::file::iovec_t const* bufs, int num_bufs, int piece, int offset, int flags, lt::storage_error& ec)
	{
		//std::cerr << "writev: " << std::endl;
		//std::cerr << num_bufs << std::endl;
		//std::cerr << piece << std::endl;
		//std::cerr << offset << std::endl;
		// std::vector<char>& data = m_file_data[piece];
		// if (data.size() < offset + size) data.resize(offset + size);
		// std::memcpy(&data[offset], buf, size);
		// return size;
		return pwritev(this->fd, bufs, num_bufs, piece * std::uint64_t(m_files.piece_length()) + offset);
	}

	// Not need
	void rename_file(int index, std::string const& new_filename, lt::storage_error& ec)
	{ assert(false); return ; }

	// 
	int move_storage(std::string const& save_path, int flags, lt::storage_error& ec) { return 0; }
	bool verify_resume_data(lt::bdecode_node const& rd
					, std::vector<std::string> const* links
					, lt::storage_error& error) { return false; }
	void write_resume_data(lt::entry& rd, lt::storage_error& ec) const { return ; }
	void set_file_priority(std::vector<boost::uint8_t> const& prio, lt::storage_error& ec) {return ;}
	lt::sha1_hash hash_for_slot(int piece, lt::partial_hash& ph, int piece_size)
	{
		int left = piece_size - ph.offset;
		assert(left >= 0);
		if (left > 0)
		{
			std::vector<char>& data = m_file_data[piece];
			// if there are padding files, those blocks will be considered
			// completed even though they haven't been written to the storage.
			// in this case, just extend the piece buffer to its full size
			// and fill it with zeroes.
			if (data.size() < piece_size) data.resize(piece_size, 0);
			ph.h.update(&data[ph.offset], left);
		}
		return ph.h.final();
	}
	void release_files(lt::storage_error& ec) { return ; }
	void delete_files(int i, lt::storage_error& ec) { return ; }

	bool tick () { return false; };


	std::map<int, std::vector<char> > m_file_data;
	lt::file_storage m_files;

	// Test for write file
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
		std::cerr << "usage: " << argv[0] << " <magnet-url> <target-partition-path>" << std::endl;
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
	atp.url = argv[1];
	//atp.ti = boost::make_shared<lt::torrent_info>(std::string(argv[1]), boost::ref(ec), 0);
	atp.save_path = argv[2]; // save in current dir
	atp.storage = temp_storage_constructor;
	//atp.storage = lt::default_storage_constructor;

	lt::torrent_handle h = ses.add_torrent(atp);
	boost::progress_display show_progress(100, std::cerr);
	unsigned long last_progess = 0, progress = 0;

	for (;;) {
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		// progress
		last_progess = progress;
		progress = h.status().progress * 100;
		show_progress += progress - last_progess;

		for (lt::alert const* a : alerts) {
			// std::cout << a->message() << std::endl;
			// if we receive the finished alert or an error, we're done
			if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
				// Start high performance seed
				lt::high_performance_seed(set);
				ses.apply_settings(set);
				std::cerr << "start seeding" << std::endl;
				goto done;
			}
			if (lt::alert_cast<lt::torrent_error_alert>(a)) {
				std::cerr << "Error" << std::endl;
				goto done;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	done:

	// seed until idle 15mins
	int timeout = 15 * 60;
	lt::torrent_status status;
	for (;;) {
		status = h.status();
		int utime = status.time_since_upload;
		int dtime = status.time_since_download;
		std::cerr << utime << " " << dtime << std::endl;
		if(utime == -1 && timeout < dtime){
			break;
		}
		else if(timeout < utime){
			// idel 15mins
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	//std::this_thread::sleep_for(std::chrono::hours(2));
	std::cout << "done, shutting down" << std::endl;
}
