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

std::string server_address = "0.0.0.0:50051";

int main(int argc, char **argv)
{
	spdlog::cfg::load_env_levels();

	lt::settings_pack p;
	// setup alert mask
	p.set_int(lt::settings_pack::alert_mask,
		lt::alert_category::error | lt::alert_category::status);

	// disable all encrypt to avoid bug https://github.com/arvidn/libtorrent/issues/6735#issuecomment-1036675263
	p.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
	p.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);

	lt::session_params ses_params(p);
	ses_params.disk_io_constructor = ezio::raw_disk_io_constructor;

	// create session and inject to daemon.
	lt::session session(ses_params);
	ezio::ezio daemon(session);

	ezio::gRPCService service(daemon);
	service.start(server_address);

	std::cout << "Server listening on " << server_address << std::endl;
	daemon.wait(10);
	std::cout << "shutdown in main" << std::endl;

	return 0;
}
