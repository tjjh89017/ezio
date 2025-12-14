#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>
#include "spdlog/cfg/env.h"

#include <libtorrent/libtorrent.hpp>

#include "daemon.hpp"
#include "service.hpp"
#include "config.hpp"
#include "raw_disk_io.hpp"
#include "log.hpp"

int main(int argc, char **argv)
{
	spdlog::cfg::load_env_levels();

	ezio::config current_config;
	current_config.parse_from_argv(argc, argv);

	std::cout << "ezio " << EZIO_VERSION << std::endl;

	lt::settings_pack p;
	// setup alert mask
	p.set_int(lt::settings_pack::alert_mask,
		lt::alert_category::error | lt::alert_category::status);

	// disable all encrypt to avoid bug https://github.com/arvidn/libtorrent/issues/6735#issuecomment-1036675263
	p.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
	p.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);

	// disable uTP for better performance
	p.set_bool(lt::settings_pack::enable_outgoing_utp, false);
	p.set_bool(lt::settings_pack::enable_incoming_utp, false);
	p.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);

	//p.set_int(lt::settings_pack::alert_mask, lt::alert_category::peer | lt::alert_category::status);
	
	// tune
	p.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::suggest_read_cache);
	p.set_int(lt::settings_pack::max_queued_disk_bytes, 128 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_not_sent_low_watermark, 524288);
	p.set_int(lt::settings_pack::send_buffer_watermark, 128 * 1024 * 1024);
	p.set_int(lt::settings_pack::send_buffer_low_watermark, 32 * 1024 * 1024);

	lt::session_params ses_params(p);
	if (!current_config.file_flag) {
		ses_params.disk_io_constructor = ezio::raw_disk_io_constructor;
	}

	// create session and inject to daemon.
	lt::session session(ses_params);
	ezio::ezio daemon(session);

	ezio::gRPCService service(daemon);
	service.start(current_config.listen_address);

	// start log
	ezio::log log(daemon);

	std::cout << "Server listening on " << current_config.listen_address << std::endl;
	daemon.wait(10);
	std::cout << "shutdown in main" << std::endl;

	log.join();
	service.stop();

	return 0;
}
