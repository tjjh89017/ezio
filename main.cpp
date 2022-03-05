#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <getopt.h>
#include <stddef.h>

#include <libtorrent/libtorrent.hpp>

#include "daemon.hpp"
#include "raw_disk_io.hpp"
#include "service.hpp"

std::string server_address = "0.0.0.0:50051";

int main(int argc, char ** argv)
{

	ezio::ezio& daemon = ezio::ezio::get_instance();
	lt::session_params ses_params;
	ses_params.disk_io_constructor = lt::default_disk_io_constructor;

	lt::session *session = new lt::session(ses_params);
	daemon.set_session(session);

	// TODO add torrent or rely on gRPC only
	ezio::gRPCService grpcservice(server_address);

	daemon.set_grpcservice(&grpcservice);

	std::cout << "Server listening on " << server_address << std::endl;
	daemon.wait(10);
	std::cout << "shutdown in main" << std::endl;

	return 0;
}
