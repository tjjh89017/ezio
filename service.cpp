#include "service.hpp"

EZIOServiceImpl::EZIOServiceImpl(lt::session &tmp) : ses(tmp) {}

Status EZIOServiceImpl::GetTorrentStatus(ServerContext* context, const UpdateRequest* request, UpdateStatus* status)
{
	// get status from session
	
	// we ignore request hashes first, always return all
	auto hash = request->hashes();
	for(auto h : hash){
		//std::cout << h << std::endl;
		// do some filter for below
	}

	auto &t_stats = *status->mutable_torrents();
	std::stringstream ss;
	std::vector<lt::torrent_handle> torrents = ses.get_torrents();
	for(lt::torrent_handle const &h : torrents) {
		auto hash = status->add_hashes();
		ss.str("");
		ss.clear();
		ss << h.info_hash();
		ss >> *hash;
		//std::cout << *hash << std::endl;

		// disable those info we don't need
		lt::torrent_status t_stat = h.status(lt::torrent_handle::query_name);

		//std::cout << t_stat.download_payload_rate << std::endl;
		//std::cout << t_stat.upload_payload_rate << std::endl;
		//std::cout << t_stat.progress << std::endl;

		Torrent t;
		t.set_hash(*hash);
		t.set_name(t_stat.name);
		t.set_progress(t_stat.progress);
		t.set_download(t_stat.download_payload_rate);
		t.set_upload(t_stat.upload_payload_rate);
		t.set_active_time(t_stat.active_time);
		t_stats[*hash] = t;
	}
	
	return Status::OK;
}

gRPCService::gRPCService(lt::session &tmp, std::string listen_address = "127.0.0.1:50051") : 
	server_address(listen_address),
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
