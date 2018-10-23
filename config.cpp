#include "config.hpp"

void config::parse_from_argv(int argc, char **argv)
{
	bpo::options_description desc("Allowed Options");
	desc.add_options()
		("help,h", "some help")
		("ratio,e", bpo::value<int>(&seed_limit_ezio), "assign seeding ratio limit")
		("timeout,t", bpo::value<int>(&timeout_ezio), "assign timeout as N min(s).")
		("contact,k",bpo::value<int>(&max_contact_tracker_times), "assign maxminum failure number to contact tracker")
		("maxu,m", bpo::value<int>(&max_upload_ezio), "assign maxminum upload number")
		("maxc,c", bpo::value<int>(&max_connection_ezio), "assign maxminum connection number")
		("sequential,s", bpo::bool_switch(&sequential_flag)->default_value(false), "enable sequential download")
		("file,f", bpo::bool_switch(&file_flag)->default_value(false), "read data from file rather than raw disk")
		("upload,U", bpo::bool_switch(&seed_flag)->default_value(false), "seed mode")
		("torrent", bpo::value<std::string>(&torrent), "")
		("save_path", bpo::value<std::string>(&save_path), "");


	bpo::positional_options_description pos_opt;
	pos_opt.add("torrent", 1).add("save_path", 1);

	bpo::variables_map vmap;
	bpo::store(bpo::command_line_parser(argc, argv)
			.options(desc)
			.positional(pos_opt)
			.run(),
			vmap);
	bpo::notify(vmap);

	if(vmap.count("help")){
		std::cout << desc << std::endl;
		exit(0);
	}
}
