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
using ezio::PauseTorrentRequest;
using ezio::PauseTorrentResponse;
using ezio::ResumeTorrentRequest;
using ezio::ResumeTorrentResponse;
using ezio::EZIO;

namespace ezio
{
class ezio;

class gRPCService final : public EZIO::Service
{
public:
	gRPCService(ezio &);
	explicit gRPCService();
	void start(std::string);
	void stop();
	void wait();


	virtual Status Shutdown(ServerContext *context, const Empty *e1, Empty *e2) override;
	virtual Status GetTorrentStatus(ServerContext *context, const UpdateRequest *request, UpdateStatus *response) override;
	virtual Status AddTorrent(ServerContext *context, const AddRequest *request, AddResponse *response) override;
	virtual Status PauseTorrent(ServerContext *context, const PauseTorrentRequest *request, PauseTorrentResponse *response) override;
	virtual Status ResumeTorrent(ServerContext *context, const ResumeTorrentRequest *request, ResumeTorrentResponse *response) override;
	virtual Status GetVersion(ServerContext *context, const Empty *e, VersionResponse *response) override;

private:
	ezio &daemon_;
	std::unique_ptr<Server> server_;
};

}  // namespace ezio

#endif
