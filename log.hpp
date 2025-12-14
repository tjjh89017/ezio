#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <thread>
#include <mutex>
#include <condition_variable>
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

	// Alert notification synchronization
	std::mutex m_alert_mutex;
	std::condition_variable m_alert_cv;
	bool m_alert_ready = false;
};

}  // namespace ezio

#endif
