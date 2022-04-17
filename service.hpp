#ifndef __SERVICE_HPP__
#define __SERVICE_HPP__

#include <iostream>
#include <sstream>
#include <vector>
#include <libtorrent/libtorrent.hpp>
#include <grpc++/grpc++.h>
#include "ezio.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using ezio::Empty;
using ezio::Torrent;
using ezio::AddRequest;
using ezio::AddResponse;
using ezio::UpdateRequest;
using ezio::UpdateStatus;
using ezio::EZIO;

namespace ezio
{
class EZIOServiceImpl final : public EZIO::Service
{
public:
	EZIOServiceImpl();
	virtual Status Shutdown(ServerContext *context, const Empty *e1, Empty *e2) override;
	virtual Status GetTorrentStatus(ServerContext *context, const UpdateRequest *request, UpdateStatus *status) override;
	virtual Status AddTorrent(ServerContext *context, const AddRequest *request, AddResponse *response) override;
};

class gRPCService
{
public:
	gRPCService(std::string);
	explicit gRPCService();
	void stop();
	void wait();

	std::string server_address;
	EZIOServiceImpl service;
	std::unique_ptr<Server> server;
};

}  // namespace ezio

#endif
