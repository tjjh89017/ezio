#include "config.hpp"

namespace ezio
{

void config::parse_from_argv(int argc, char **argv)
{
	// clang-format off
	bpo::options_description desc("Allowed Options");
	desc.add_options()
		("help,h", "some help")
		("file,F", bpo::bool_switch(&file_flag)->default_value(false), "read data from file rather than raw disk")
		("listen,l", bpo::value<std::string>(&listen_address), "gRPC service listen address and port, default is 127.0.0.1:50051")
		("cache-size", bpo::value<int>(&cache_size_mb)->default_value(4096), "unified cache size in MB, default is 4096")
		("aio-threads", bpo::value<int>(&aio_threads)->default_value(16), "number of threads for disk I/O, default is 16")
		("hashing-threads", bpo::value<int>(&hashing_threads)->default_value(8), "number of threads for hashing, default is 8")
		("version,v", "show version")
	;
	// clang-format on

	// clang-format off
	bpo::variables_map vmap;
	bpo::store(bpo::command_line_parser(argc, argv)
		.options(desc)
		.run(),
		vmap);
	bpo::notify(vmap);
	// clang-format on

	if (vmap.count("help")) {
		std::cout << desc << std::endl;
		exit(0);
	}

	if (vmap.count("version")) {
		std::cout << "ezio " << EZIO_VERSION << std::endl;
		exit(0);
	}
}

}  // namespace ezio
