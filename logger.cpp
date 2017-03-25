#include "logger.hpp"

Logger::Logger() {

	this->log.open("ezio.log", std::ios::out);
}

Logger::~Logger() {

	// flush out
	this->log << this->buffer.str() << std::flush;
	this->buffer.str("");
	this->log.close();
}

Logger& Logger::getInstance() {
	static Logger instance;

	return instance;
}

std::ostream& Logger::info() {

	if(this->buffer.tellp() > Logger::MAX_BUFFER) {
		this->log << this->buffer.str() << std::flush;
		this->buffer.str("");
	}
	return this->buffer;
}

std::ostream& Logger::debug() {

	if(this->buffer.tellp() > Logger::MAX_BUFFER) {
		this->log << this->buffer.str() << std::flush;
		this->buffer.str("");
	}
	return this->buffer;
}
