#ifndef __APP_HPP__
#define __APP_HPP__

#include <boost/core/noncopyable.hpp>
#include <libtorrent/libtorrent.hpp>

#include "config.hpp"
#include "daemon.hpp"
#include "log.hpp"
#include "service.hpp"

namespace ezio
{

class app : boost::noncopyable
{
public:
	explicit app(const config &cfg);

	int run();

private:
	static lt::session_params make_session_params(const config &cfg);

	config m_config;
	lt::session m_session;
	ezio m_daemon;
	gRPCService m_service;
	log m_log;
};

}  // namespace ezio

#endif
