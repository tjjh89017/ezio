#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>

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
#ifdef ENABLE_GRPC
#include "service.hpp"
#endif

namespace lt = libtorrent;

/*
int timeout_ezio = 15; // Default timeout (min)
int seed_limit_ezio = 3; // Default seeding ratio limit
int max_upload_ezio = 4;
int max_connection_ezio = max_upload_ezio + 2;
*/

int main(int argc, char ** argv)
{

	// need to remove
	int log_flag = 0;

	config current;
	current.parse_from_argv(argc, argv);

	lt::session ses;
	lt::error_code ec;
	lt::settings_pack set;

	// setting
	lt::high_performance_seed(set);
	// we don't need DHT
	set.set_bool(lt::settings_pack::enable_dht, false);

	// tuning cache
	set.set_bool(lt::settings_pack::volatile_read_cache, true);
	set.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::suggest_read_cache);
	set.set_int(lt::settings_pack::send_buffer_watermark, 128 * 1024 * 1024);
	set.set_int(lt::settings_pack::send_buffer_watermark_factor, 150);
	set.set_int(lt::settings_pack::send_buffer_low_watermark, 40 * 1024 * 1024);

	// threads
	set.set_int(lt::settings_pack::aio_threads, 16);

	// Cache size if non-zero, in KiB
	// TODO change unit to MiB
	if(current.cache_size >= 0) {
		int size = (int)(current.cache_size / 16);
		set.set_int(lt::settings_pack::cache_size, size);
	}

	// apply seeting to libtorrent
	ses.apply_settings(set);

#ifdef ENABLE_GRPC
	gRPCService grpcservice(ses, current.listen_address);
#endif

	std::string logfile = "";
	const boost::int64_t fileSizeLimit = 100 * 1024 * 1024;
	char *buffer = (char*)malloc(fileSizeLimit);
	for(auto torrent = current.torrents.begin(), save_path = current.save_paths.begin();
			torrent != current.torrents.end() && save_path != current.save_paths.end();
			torrent++, save_path++){
		lt::add_torrent_params atp;
		std::string bt_info = *torrent;
		atp.save_path = *save_path;
		if(current.seed_flag){
			atp.flags |= atp.flag_seed_mode;
		}

		if (current.sequential_flag) {
		  std::cout << "//NOTE// Sequential download is enabled!" << std::endl;
		}

		// magnet or torrent
		// TODO find a better way
		// TODO migrate to Libtorrent>=1.2.x
		if(bt_info.substr(bt_info.length() - 8, 8) == ".torrent"){
			FILE *fp = fopen(bt_info.c_str(), "rb");
			boost::int64_t size = fread(buffer, 1, fileSizeLimit, fp);
			const int depthLimit = 100;
			const int tokenLimit = 10000000;
			lt::bdecode_node node;
			bdecode(buffer, buffer + size, node, ec, nullptr, depthLimit, tokenLimit);
			atp.ti = boost::make_shared<lt::torrent_info>(node, boost::ref(ec));
			//atp.ti = boost::make_shared<lt::torrent_info>(bt_info, boost::ref(ec), 0);
			//std::cerr << "test" << ec << std::endl;
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

	}
	free(buffer);

	unsigned long progress = 0;
	lt::torrent_status status;

	Logger *log;
	if(log_flag){
		Logger::setLogFile(logfile);
		log = &Logger::getInstance();
	}

	std::cout << "Start downloading" << std::endl;

	auto torrents = ses.get_torrents();
	for(;;){
		std::vector<lt::alert*> alerts;
		ses.pop_alerts(&alerts);

		int download_rate = 0, upload_rate = 0;
		progress = 0;
		for(auto handle : torrents){
			handle.force_reannounce();
			status = handle.status();
			download_rate += status.download_payload_rate;
			upload_rate += status.upload_payload_rate;
			progress += status.progress * 100;
		}
		// progress
		progress /= torrents.size();
		status = torrents[0].status();
		//show_progress += progress - last_progess;
		std::cout << std::fixed << "\r"
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)download_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[DT: " << (int)status.active_time  << " secs] "
			<< "[U: " << std::setprecision(2) << (float)upload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[UT: " << (int)status.seeding_time  << " secs] "
			<< std::flush;

		// Log info
		if(log_flag){
			log->info() << "time=" << boost::posix_time::second_clock::local_time() << std::endl;
			log->info() << "download_payload_rate=" << status.download_payload_rate << std::endl;
			log->info() << "upload_payload_rate=" << status.upload_payload_rate << std::endl;

			std::vector<lt::peer_info> peers;
			//handle.get_peer_info(peers);

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
				// check all torrents
				bool all_done = true;
				for(auto handle : torrents){
					status = handle.status();
					if(!status.is_finished)
						all_done = false;
						break;
				}
				if(all_done){
					goto done;
				}
			}
			if (lt::alert_cast<lt::torrent_error_alert>(a)) {
				std::cerr << "Error" << std::endl;
				std::cerr << a->what() << std::endl;
				std::cerr << a->message() << std::endl;
				return 1;
			}
		}
		// check all torrents
		bool all_done = true;
		for(auto handle : torrents){
			status = handle.status();
			if(!status.is_finished)
				all_done = false;
				break;
		}
		if(all_done){
			goto done;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	done:
	std::cout << std::endl;

	// display avg download speed
	for (auto handle : torrents) {
		status = handle.status();
		if (status.active_time == 0)
			continue;
		std::cout << std::fixed << std::setprecision(2)
			<< std::endl
			<< "Avg D: " << (double)status.total_done / 1024 / 1024 / 1024 / status.active_time * 60 << " GB/min, "
			<< (double)status.total_done / 1024 / 1024 / status.active_time  << " MB/s" << std::endl;
	}

	// Start high performance seed
	ses.apply_settings(set);

	// seed until idle (secs)
	int timeout = current.timeout_ezio * 60;

	// seed until seed rate
	boost::int64_t seeding_rate_limit = current.seed_limit_ezio;

	// wait for seed mode to start
	std::this_thread::sleep_for(std::chrono::seconds(3));

	torrents = ses.get_torrents();

	for (;;) {
		int upload_rate = 0;
		for(auto handle : torrents){
			handle.force_reannounce();
			status = handle.status();
			upload_rate += status.upload_payload_rate;
		}
		status = torrents[0].status();
		std::cout << std::fixed << "\r"
			/*
			<< "[P: " << progress << "%] "
			<< "[D: " << std::setprecision(2) << (float)status.download_payload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[T: " << (int)status.active_time  << " secs] "
			*/
			<< "[U: " << std::setprecision(2) << (float)upload_rate / 1024 / 1024 /1024 * 60 << " GB/min] "
			<< "[T: " << (int)status.seeding_time  << " secs] "
			//<< status.state
			<< std::flush;

		bool all_done = true;
		for(auto handle : torrents){
			status = handle.status();
			int utime = status.time_since_upload;
			int dtime = status.time_since_download;
			boost::int64_t total_size = handle.torrent_file()->total_size();
			boost::int64_t total_payload_upload = status.total_payload_upload;

			handle.scrape_tracker();

			// in force seed mode, never pause any torrent
			if(current.seed_flag){
				all_done = false;
				continue;
			}

			// we don't need to check who is paused already
			if(status.paused){
				continue;
			}

			all_done = false;
			if(utime == -1 && timeout < dtime){
				handle.auto_managed(false);
				handle.pause();
			}
			else if(timeout < utime){
				handle.auto_managed(false);
				handle.pause();
			}
			else if(seeding_rate_limit < (total_payload_upload / total_size)){
				handle.auto_managed(false);
				handle.pause();
			}
		}
		if(all_done){
			goto finish;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	finish:
	std::cout << "\nDone, shutting down" << std::endl;

#ifdef ENABLE_GRPC
	grpcservice.stop();
#endif

	return 0;
}
