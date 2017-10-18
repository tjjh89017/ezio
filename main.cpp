#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>

#ifdef __linux__
#include <sys/sysinfo.h>
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

#include "core_ext.hpp"
#include "logger.hpp"
#include "storage.hpp"

namespace lt = libtorrent;
using raw_storage = ezio::storage::raw_storage;

int timeout_ezio = 15; // Default timeout (min)
int seed_limit_ezio = 3; // Default seeding ratio limit
int max_upload_ezio = 4;
int max_connection_ezio = max_upload_ezio + 2;
int max_contact_tracker_times = 30; // Max error times for scrape tracker

lt::storage_interface* raw_storage_constructor(lt::storage_params const& params)
{
	return new raw_storage(*params.files, params.path);
}

void usage()
{
	std::cerr << "Usage: ezio [OPTIONS] <magnet-url/torrent-file> <target-partition-path>\n"
		<< "OPTIONS:\n"
		<< "	-e N: assign seeding ratio limit as N. Default value is " << seed_limit_ezio << "\n"
		<< "	-k N: assign maxminum failure number to contact tracker as N. Default value is " << max_contact_tracker_times<< "\n"
		<< "	-m N: assign maxminum upload number as N. Default value is " << max_upload_ezio << "\n"
		<< "	-c N: assign maxminum connection number as N. Default value is " << max_upload_ezio + 2 << "\n"
		<< "	-s: enable sequential download\n"
		<< "	-t N: assign timeout as N min(s). Default value " << timeout_ezio << "\n"
		<< "	-l file: assign log file"
		<< std::endl;
}

int main(int argc, char ** argv)
{
	lt::add_torrent_params atp;
	int opt;
	int opt_n = 0;
	int seq_flag = 0;
	int log_flag = 0;
	std::string logfile = "";

	opterr = 0;
	while ((opt = getopt(argc, argv, "e:m:c:st:l:")) != -1) {
		switch (opt) {
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
			case 'c':
				max_connection_ezio = atoi(optarg);
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
			case 'l':
				logfile = optarg;
				log_flag = 1;
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
	if (sysinfo(&info) == 0) {
		unsigned long totalram = info.totalram * info.mem_unit;
		if (totalram > 2_GiB) {
			// unit: blocks per 16KiB
			int size = (int)(totalram / 2 / 16_KiB);
			set.set_int(lt::settings_pack::cache_size, size);
		}
	}
#endif
	ses.apply_settings(set);

	// magnet or torrent
	// TODO find a better way
	if (bt_info.substr(bt_info.length() - 8, 8) == ".torrent")
		atp.ti = boost::make_shared<lt::torrent_info>(bt_info, boost::ref(ec), 0);
	else
		atp.url = bt_info;
	atp.storage = raw_storage_constructor;

	lt::torrent_handle handle = ses.add_torrent(atp);
	handle.set_max_uploads(max_upload_ezio);
	handle.set_max_connections(max_connection_ezio);
	handle.set_sequential_download(seq_flag);
	//boost::progress_display show_progress(100, std::cout);
	unsigned long last_progess = 0, progress = 0;
	lt::torrent_status status;

	Logger *log;
	if (log_flag) {
		Logger::setLogFile(logfile);
		log = &Logger::getInstance();
	}

	std::cout << "Start downloading" << std::endl;

	for (;;) {
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		status = handle.status();
		// progress
		last_progess = progress;
		progress = status.progress * 100;
		//show_progress += progress - last_progess;
		std::cout << std::fixed << "\r"
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float) status.download_payload_rate / 1_GiB * 1_min << " GiB/min] "
			<< "[DT: " << (int) status.active_time  << " secs] "
			<< "[U: " << std::setprecision(2) << (float) status.upload_payload_rate / 1_GiB * 1_min << " GiB/min] "
			<< "[UT: " << (int) status.seeding_time  << " secs] "
			<< std::flush;

		// Log info
		if (log_flag) {
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

		if (status.is_finished) goto done;

		for (lt::alert const* a : alerts) {
			// std::cout << a->message() << std::endl;
			// if we receive the finished alert or an error, we're done
			if (lt::alert_cast<lt::torrent_finished_alert>(a)) goto done;

			if (auto *error_alert = lt::alert_cast<lt::torrent_error_alert>(a)) {
				const auto ecode = error_alert->error;
				if (error_alert->error.category() == ezio::errors::category()) { // Handling ezio errors
					std::cerr << std::endl << "Error: " << ecode.message() << std::endl;
				} else {
					std::cerr << std::endl << "Error<from " << ecode.category().name() << ">: " << error_alert->message() << std::endl;
				}
				return 1;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
	done:
	std::cout << std::endl;

	// Start high performance seed
	lt::high_performance_seed(set);
	ses.apply_settings(set);
	std::cout << "Start high-performance seeding" << std::endl;

	// seed until idle (secs)
	int timeout = timeout_ezio * 1_min;

	// seed until seed rate
	boost::int64_t seeding_rate_limit = seed_limit_ezio;
	boost::int64_t total_size = handle.torrent_file()->total_size();

	int fail_contact_tracker = 0;
	for (;;) {
		status = handle.status();
		int utime = status.time_since_upload;
		int dtime = status.time_since_download;
		boost::int64_t total_payload_upload = status.total_payload_upload;
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		std::cout << std::fixed << "\r"
			<< "[U: " << std::setprecision(2) << (float) status.upload_payload_rate / 1_GiB * 1_min << " GiB/min] "
			<< "[T: " << (int) status.seeding_time << " secs] "
			<< std::flush;

		if (utime == -1 && timeout < dtime) {
			break;
		} else if (timeout < utime) {
			break;
		} else if (seeding_rate_limit < (total_payload_upload / total_size)) {
			break;
		}

		handle.scrape_tracker();
		for (lt::alert const* a : alerts) {
			if (lt::alert_cast<lt::scrape_failed_alert>(a)) {
				++fail_contact_tracker;
			}
		}

		if (fail_contact_tracker > max_contact_tracker_times) {
			std::cout << std::endl << "Tracker is gone! Finish seeding!" << std::endl;
			break;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	std::cout << std::endl << "Done, shutting down" << std::endl;

	return 0;
}
