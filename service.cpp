#include "service.hpp"
#include "daemon.hpp"

namespace ezio {

gRPCService::gRPCService(std::string listen_address = "0.0.0.0:50051")
  : server_address(listen_address)
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

void gRPCService::wait()
{
  server->Wait();
}

EZIOServiceImpl::EZIOServiceImpl()
{}

Status EZIOServiceImpl::Shutdown(ServerContext *context, const Empty *e1,
                                 Empty *e2)
{
  // TODO fix this
  std::cout << "shutdown" << std::endl;

  ezio &daemon = ezio::ezio::get_instance();
  daemon.stop();
  return Status::OK;
}

Status EZIOServiceImpl::GetTorrentStatus(ServerContext *context,
                                         const UpdateRequest *request,
                                         UpdateStatus *status)
{
  // TODO fix this
  std::cout << "GetTorrentStatus" << std::endl;

  ezio &daemon = ezio::ezio::get_instance();
  lt::session &ses = *daemon.get_session();

  // TODO we ignore request hashes first, always return all
  auto hash = request->hashes();
  for(auto h : hash) {
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
    lt::torrent_status t_stat = h.status();

    //std::cout << t_stat.download_payload_rate << std::endl;
    //std::cout << t_stat.upload_payload_rate << std::endl;
    //std::cout << t_stat.progress << std::endl;

    Torrent t;
    t.set_hash(*hash);
    t.set_name(t_stat.name);
    t.set_progress(t_stat.progress);
    t.set_download_rate(t_stat.download_payload_rate);
    t.set_upload_rate(t_stat.upload_payload_rate);
    t.set_is_finished(t_stat.is_finished);
    t.set_num_peers(t_stat.num_peers);
    t_stats[*hash] = t;
  }

  return Status::OK;
}

Status EZIOServiceImpl::AddTorrent(ServerContext *context,
                                   const AddRequest *request,
                                   AddResponse *response)
{
  std::cout << "AddTorrent" << std::endl;

  ezio &daemon = ezio::ezio::get_instance();
  lt::session &ses = *daemon.get_session();

  const std::string torrent = request->torrent();
  lt::span<const char> torrent_byte(torrent);

  lt::add_torrent_params atp;
  lt::error_code ec;
  const int depth_limit = 100;
  const int token_limit = 10000000;
  const lt::bdecode_node node =
    lt::bdecode(torrent_byte, ec, nullptr, depth_limit, token_limit);

  if(ec) {
    return Status(grpc::StatusCode::UNAVAILABLE, "failed to decode node");
  }

  atp.ti = std::make_shared<lt::torrent_info>(node, std::ref(ec));
  atp.save_path = request->save_path();

  if(ec) {
    return Status(grpc::StatusCode::UNAVAILABLE, "failed to save path");
  }

  lt::torrent_handle handle = ses.add_torrent(std::move(atp));

  // TODO need to setup this via gRPC
  handle.set_max_uploads(3);
  handle.set_max_connections(5);

  // TODO fix with logger
  std::cout << "torrent added." << std::endl;

  return Status::OK;
}

} // namespace ezio
