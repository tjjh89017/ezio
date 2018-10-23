#include "service.hpp"

EZIOServiceImpl::EZIOServiceImpl(lt::session &tmp) : ses(tmp) {}

Status EZIOServiceImpl::GetTorrentStatus(ServerContext* context, const UpdateRequest* request, UpdateStatus* status)
{
	// get status from session
	
	// we ignore request hashs first, always return all
	auto hash = request->hashs();
	for(auto h : hash){
		std::cout << h << std::endl;
	}
	
	std::stringstream ss;
	std::vector<lt::torrent_handle> torrents = ses.get_torrents();
	for(lt::torrent_handle const &h : torrents) {
		auto hash = status->add_hashs();
		ss << h.info_hash();
		ss >> *hash;
		std::cout << *hash << std::endl;
	}
	
	return Status::OK;
}

gRPCService::gRPCService(lt::session &tmp) : 
	server_address("0.0.0.0:50051"),
	service(tmp)
{
	ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);
	server = builder.BuildAndStart();
}

void gRPCService::stop()
{
	server->Shutdown();
}
