#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <thread>
#include "daemon.hpp"

namespace ezio
{
class log
{
public:
	log(ezio &daemon);
	~log();

	void join();

	// worker function
	void report_speed();
	void report_alert();

private:
	std::thread m_speed;
	std::thread m_alert;

	ezio &m_daemon;
};

}  // namespace ezio

#endif
