#ifndef __DAEMON_HPP__
#define __DAEMON_HPP__

#include <atomic>
#include <libtorrent/libtorrent.hpp>
#include "service.hpp"

namespace ezio
{
class ezio
{
public:
	static ezio &get_instance();

	void stop();
	void wait(int sec);

	void set_session(std::unique_ptr<lt::session>);
	lt::session *get_session();
	void set_grpcservice(std::unique_ptr<gRPCService>);
	gRPCService *get_grpcservice();

private:
	ezio();
	~ezio() = default;
	ezio(const ezio &) = delete;
	ezio &operator=(const ezio &) = delete;

	std::atomic_bool shutdown_;
	std::unique_ptr<lt::session> session_;
	std::unique_ptr<gRPCService> grpcservice_;
};

}  // namespace ezio

#endif
