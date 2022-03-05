#ifndef __DAEMON_HPP__
#define __DAEMON_HPP__

#include <libtorrent/libtorrent.hpp>
#include "service.hpp"

namespace ezio {

class ezio {
public:
	static ezio& get_instance();
	
	void stop();
	void wait(int sec);

	void set_session(lt::session*);
	lt::session* get_session();
	void set_grpcservice(gRPCService*);
	gRPCService* get_grpcservice();

private:
	ezio();
	~ezio() = default;
	ezio(const ezio&) = delete;
	ezio& operator=(const ezio&) = delete;
	
	bool shutdown = false;
	lt::session *session;
	gRPCService *grpcservice;
};

} // namespace ezio


#endif
