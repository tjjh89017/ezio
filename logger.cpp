#include "logger.hpp"

Logger::Logger() {

	this->log.open("ezio.log");
}

Logger& Logger::getInstance() {
	static Logger instance;

	return instance;
}

std::ostream& Logger::info() {
	
	return this->log;
}

std::ostream& Logger::debug() {

	return this->log;
}
