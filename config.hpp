#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

#define BOOST_PROGRAM_OPTIONS_DYN_LINK 1
#include <boost/program_options.hpp>

#include <iostream>
#include <vector>

namespace bpo = boost::program_options;

namespace ezio
{

class config
{
public:
	void parse_from_argv(int argc, char **argv);

	// regular file mode
	bool file_flag = false;
	// --listen address
	std::string listen_address = "127.0.0.1:50051";
	// cache size in MB
	int cache_size_mb = 512;  // default 512MB
	// thread pool size (used for both I/O and hashing)
	int aio_threads = 16;  // default 16 threads for disk I/O
	// enable session-wide slow-start upload ramp
	bool slow_start = false;
	// slow-start step period in seconds
	int slow_start_period = 10;	 // default 10 seconds
	// BitTorrent peer listen port (0 = use libtorrent default, leave unchanged)
	int bt_listen_port = 0;
};

}  // namespace ezio

#endif
