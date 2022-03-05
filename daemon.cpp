#include <thread>
#include <chrono>
#include "daemon.hpp"

namespace ezio {

ezio::ezio() : session(nullptr)
{

}

ezio& ezio::get_instance()
{
	static ezio instance;
	return instance;
}

void ezio::stop()
{
	shutdown = true;
}

void ezio::wait(int sec)
{
	while (!shutdown) {
		std::this_thread::sleep_for(std::chrono::seconds(sec));
	}

	grpcservice->stop();
}

void ezio::set_session(lt::session* s)
{
	session = s;
}

lt::session* ezio::get_session()
{
	return session;
}

void ezio::set_grpcservice(gRPCService* g)
{
	grpcservice = g;
}

gRPCService* ezio::get_grpcservice()
{
	return grpcservice;
}

} // namespace ezio
