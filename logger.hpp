#ifndef __LOGGER_HPP__
#define __LOGGER_HPP__
#define BOOST_LOG_DYN_LINK 1

#include <iostream>
#include <fstream>
#include <sstream>

class Logger {

public:

    static Logger& getInstance();
    static void setLogFile(std::string);
    ~Logger();

    std::ostream& info();
    std::ostream& debug();

private:

    Logger();

    // dont implement
    Logger(Logger const&) {};
    void operator=(Logger const&) {};

    std::fstream log;
    std::stringstream buffer;
    static std::string logfile;

    // buffer 1MiB
    const static long MAX_BUFFER = 1024 * 1024;
};

#endif
