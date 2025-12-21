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
	int cache_size_mb = 4096;  // default 4GB for testing
	// thread pool sizes
	int aio_threads = 16;  // default 16 threads for disk I/O
	int hashing_threads = 8;  // default 8 threads for hashing
};

}  // namespace ezio

#endif
