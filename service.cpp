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
	// set recv unlimit
	builder.SetMaxReceiveMessageSize(-1);
	builder.RegisterService(this);
	server_ = builder.BuildAndStart();
}

void gRPCService::stop()
{
	server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
}

void gRPCService::wait()
{
	server_->Wait();
}

Status gRPCService::Shutdown(ServerContext *context, const Empty *e1,
	Empty *e2)
{
	spdlog::info("shutdown");

	daemon_.stop();
	return Status::OK;
}

Status gRPCService::GetTorrentStatus(ServerContext *context,
	const UpdateRequest *request,
	UpdateStatus *response)
{
	spdlog::debug("GetTorrentStatus request: {}", request->DebugString());

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
		t.set_finished_time(t_stat.finished_time);
		t.set_seeding_time(t_stat.seeding_time);
		t.set_total_payload_download(t_stat.total_payload_download);
		t.set_total_payload_upload(t_stat.total_payload_upload);
		t.set_is_paused(t_stat.is_paused);
		t.set_save_path(t_stat.save_path);
		t.set_last_upload(t_stat.last_upload);
		t.set_last_download(t_stat.last_download);

		response->mutable_torrents()->insert({hash, t});
	}

	return Status::OK;
}

Status gRPCService::AddTorrent(ServerContext *context,
	const AddRequest *request,
	AddResponse *response)
{
	spdlog::info("AddTorrent");

	try {
		daemon_.add_torrent(request->torrent(), request->save_path(), request->seeding_mode(), request->max_uploads(), request->max_connections(), request->sequential_download());
	} catch (const std::exception &e) {
		return Status(grpc::StatusCode::UNAVAILABLE, e.what());
	}

	return Status::OK;
}

Status gRPCService::PauseTorrent(ServerContext *context, const PauseTorrentRequest *request, PauseTorrentResponse *response)
{
	spdlog::info("PauseTorrent");

	try {
		daemon_.pause_torrent(request->hash());
	} catch (const std::exception &e) {
		return Status(grpc::StatusCode::UNAVAILABLE, e.what());
	}

	return Status::OK;
}

Status gRPCService::ResumeTorrent(ServerContext *context, const ResumeTorrentRequest *request, ResumeTorrentResponse *response)
{
	spdlog::info("ResumeTorrent");

	try {
		daemon_.resume_torrent(request->hash());
	} catch (const std::exception &e) {
		return Status(grpc::StatusCode::UNAVAILABLE, e.what());
	}

	return Status::OK;
}

Status gRPCService::GetVersion(ServerContext *context, const Empty *e, VersionResponse *response)
{
	spdlog::info("GetVersion");

	response->set_version(EZIO_VERSION);
	return Status::OK;
}

}  // namespace ezio
