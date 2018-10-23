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
#include <libtorrent/peer_info.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "logger.hpp"
#include "raw_storage.hpp"
#include "config.hpp"

namespace lt = libtorrent;

/*
int timeout_ezio = 15; // Default timeout (min)
int seed_limit_ezio = 3; // Default seeding ratio limit
int max_upload_ezio = 4;
int max_connection_ezio = max_upload_ezio + 2;
int max_contact_tracker_times = 30; // Max error times for scrape tracker
*/

int main(int argc, char ** argv)
{

	// need to remove
	int log_flag = 0;

	config current;
	current.parse_from_argv(argc, argv);

	lt::add_torrent_params atp;
	std::string logfile = "";
	std::string bt_info = current.torrent;
	atp.save_path = current.save_path;
	if(current.seed_flag){
		atp.flags |= atp.flag_seed_mode;
	}

	if (current.sequential_flag) {
	  std::cout << "//NOTE// Sequential download is enabled!" << std::endl;
	}

	lt::session ses;
	lt::error_code ec;
	lt::settings_pack set;

	// setting
	lt::high_performance_seed(set);
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

	if(current.file_flag == 0){
		atp.storage = raw_storage::raw_storage_constructor;
	}

	lt::torrent_handle handle = ses.add_torrent(atp);
	handle.set_max_uploads(current.max_upload_ezio);
	handle.set_max_connections(current.max_connection_ezio);
	handle.set_sequential_download(current.sequential_flag);
	//boost::progress_display show_progress(100, std::cout);
	unsigned long last_progess = 0, progress = 0;
	lt::torrent_status status;

	Logger *log;
	if(log_flag){
		Logger::setLogFile(logfile);
		log = &Logger::getInstance();
	}

	std::cout << "Start downloading" << std::endl;

	for(;;){
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		status = handle.status();
		handle.force_reannounce();
		// progress
		last_progess = progress;
		progress = status.progress * 100;
		//show_progress += progress - last_progess;
		std::cout << std::fixed << "\r"
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)status.download_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[DT: " << (int)status.active_time  << " secs] "
			<< "[U: " << std::setprecision(2) << (float)status.upload_payload_rate / 1024 / 1024 /1024 *60 << " GB/min] "
			<< "[UT: " << (int)status.seeding_time  << " secs] "
			<< std::flush;

		// Log info
		if(log_flag){
			log->info() << "time=" << boost::posix_time::second_clock::local_time() << std::endl;
			log->info() << "download_payload_rate=" << status.download_payload_rate << std::endl;
			log->info() << "upload_payload_rate=" << status.upload_payload_rate << std::endl;

			std::vector<lt::peer_info> peers;
			handle.get_peer_info(peers);

			for (auto peer : peers) {
				log->info() << "ip=" << peer.ip.address().to_string() << std::endl
					<< "payload_up_speed=" << peer.payload_up_speed << std::endl
					<< "payload_down_speed=" << peer.payload_down_speed << std::endl;
			}
		}

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
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
	done:
	std::cout << std::endl;


	// Start high performance seed
	ses.apply_settings(set);

	// seed until idle (secs)
	int timeout = current.timeout_ezio * 60;

	// seed until seed rate
	boost::int64_t seeding_rate_limit = current.seed_limit_ezio;
	boost::int64_t total_size = handle.torrent_file()->total_size();

	int fail_contact_tracker = 0;
	for (;;) {
		status = handle.status();
		handle.force_reannounce();
		int utime = status.time_since_upload;
		int dtime = status.time_since_download;
		boost::int64_t total_payload_upload = status.total_payload_upload;
		// ses.set_alert_mask(lt::alert::tracker_notification | lt::alert::error_notification);
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		std::cout << std::fixed << "\r"
			/*
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)status.download_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[T: " << (int)status.active_time  << " secs] "
			*/
			<< "[U: " << std::setprecision(2) << (float)status.upload_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[T: " << (int)status.seeding_time  << " secs] "
			<< status.state
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

		if(fail_contact_tracker > current.max_contact_tracker_times){
	                std::cout << "\nTracker is gone! Finish seeding!" << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	std::cout << "\nDone, shutting down" << std::endl;
	return 0;
}
