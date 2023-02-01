#include "service.hpp"
#include <spdlog/spdlog.h>
#include "daemon.hpp"

namespace ezio
{
gRPCService::gRPCService(ezio &daemon) :
	daemon_(daemon)
{
}

void gRPCService::start(std::string listen_address)
{
	ServerBuilder builder;
	builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
	builder.RegisterService(this);
	server_ = builder.BuildAndStart();
}

void gRPCService::stop()
{
	server_->Shutdown();
}

void gRPCService::wait()
{
	server_->Wait();
}

Status gRPCService::Shutdown(ServerContext *context, const Empty *e1,
	Empty *e2)
{
	SPDLOG_DEBUG("shutdown");

	daemon_.stop();
	SPDLOG_DEBUG("shutdown2");
	stop();
	SPDLOG_DEBUG("shutdown3");
	return Status::OK;
}

Status gRPCService::GetTorrentStatus(ServerContext *context,
	const UpdateRequest *request,
	UpdateStatus *response)
{
	SPDLOG_DEBUG("GetTorrentStatus request: {}", request->DebugString());

	std::vector<std::string> hashes(request->hashes().begin(), request->hashes().end());
	auto result = daemon_.get_torrent_status(hashes);
	for (const auto &iter : result) {
		const auto &hash = iter.first;
		const auto &t_stat = iter.second;

		response->add_hashes(hash);

		Torrent t;
		t.set_hash(hash);
		t.set_name(t_stat.name);
		t.set_progress(t_stat.progress);
		t.set_download_rate(t_stat.download_rate);
		t.set_upload_rate(t_stat.upload_rate);
		t.set_is_finished(t_stat.is_finished);
		t.set_active_time(t_stat.active_time);
		t.set_num_peers(t_stat.num_peers);
		t.set_state(t_stat.state);
		t.set_total_done(t_stat.total_done);
		t.set_total(t_stat.total);
		t.set_num_pieces(t_stat.num_pieces);
		response->mutable_torrents()->insert({hash, t});
	}

	return Status::OK;
}

Status gRPCService::AddTorrent(ServerContext *context,
	const AddRequest *request,
	AddResponse *response)
{
	SPDLOG_INFO("AddTorrent");

	try {
		daemon_.add_torrent(request->torrent(), request->save_path());
	} catch (const std::exception &e) {
		return Status(grpc::StatusCode::UNAVAILABLE, e.what());
	}

	return Status::OK;
}

}  // namespace ezio
