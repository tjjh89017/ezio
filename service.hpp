#ifndef __SERVICE_HPP__
#define __SERVICE_HPP__

#include <iostream>
#include <sstream>
#include <vector>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <grpc++/grpc++.h>
#include "ezio.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using ezio::Torrent;
using ezio::UpdateStatus;
using ezio::UpdateRequest;
using ezio::EZIO;

namespace lt = libtorrent;
class EZIOServiceImpl final : public EZIO::Service {
public:
	EZIOServiceImpl(lt::session &tmp);
	virtual Status GetTorrentStatus(ServerContext* context, const UpdateRequest* request, UpdateStatus* status) override;
	lt::session &ses;
};

void grpc_start(lt::session&);

class gRPCService {
public:
	gRPCService(lt::session&, std::string);
	//void start();
	void stop();

	std::string server_address;
	EZIOServiceImpl service;
	std::unique_ptr<Server> server;
};

#endif
