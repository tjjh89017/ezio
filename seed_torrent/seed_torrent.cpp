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
	set.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6666");
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
	atp.flags |= atp.flag_seed_mode;

	lt::torrent_handle handle = ses.add_torrent(atp);
	//handle.set_max_uploads(4);
	//handle.set_sequential_download(1);
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
				std::cerr << a->message() << std::endl;
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
