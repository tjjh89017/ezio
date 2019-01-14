#ifndef __CONFIG_HPP__
#define __CONFIG_HPP__

#define BOOST_PROGRAM_OPTIONS_DYN_LINK 1
#include <boost/program_options.hpp>

#include <iostream>
#include <vector>

namespace bpo = boost::program_options;

class config {

public:
	void parse_from_argv(int argc, char **argv);

	// -t
	int timeout_ezio = 15; // Default timeout (min)
	// -e
	int seed_limit_ezio = 3; // Default seeding ratio limit
	// -m
	int max_upload_ezio = 4;
	// -c
	int max_connection_ezio = max_upload_ezio + 2;
	// -k
	int max_contact_tracker_times = 30; // Max error times for scrape tracker
	// -s
	bool sequential_flag = false;
	// -U
	bool seed_flag = false;
	// -f
	bool file_flag = false;
	// --cache in KiB
	int cache_size = -1;

	std::vector<std::string> torrents;
	std::vector<std::string> save_paths;
	
	// torrent name
	std::string legacy_torrent;
	// storage to
	std::string legacy_save_path;
};

#endif
