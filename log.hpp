#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "daemon.hpp"

namespace ezio
{
class log
{
public:
	log(ezio &daemon, lt::io_context &ioc);

	void start();

private:
	void schedule_speed_report();
	void process_alerts();
	void shutdown();

	ezio &m_daemon;
	lt::io_context &m_ioc;
	boost::asio::steady_timer m_speed_timer;
};

}  // namespace ezio

#endif
