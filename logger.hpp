#ifndef __LOGGER_HPP__
#define __LOGGER_HPP__
#define BOOST_LOG_DYN_LINK 1

#include <iostream>
#include <fstream>

class Logger {

public:

	static Logger& getInstance();

	std::ostream& info();
	std::ostream& debug();

private:

	Logger();

	// dont implement
	Logger(Logger const&){};
	void operator=(Logger const&){};

	std::fstream log;
};

#endif
