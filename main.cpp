#include <iostream>

#include "spdlog/cfg/env.h"

#include "app.hpp"
#include "config.hpp"
#include "version.hpp"

int main(int argc, char **argv)
{
	spdlog::cfg::load_env_levels();

	ezio::config cfg;
	cfg.parse_from_argv(argc, argv);

	std::cout << "ezio " << EZIO_VERSION << std::endl;

	ezio::app app(cfg);
	return app.run();
}
