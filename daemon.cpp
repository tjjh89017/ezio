#include <thread>
#include <chrono>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include "daemon.hpp"

namespace ezio
{
ezio::ezio(lt::session &session) :
	session_(session)
{
}

void ezio::stop()
{
	shutdown_ = true;
}

void ezio::wait(int interval_second)
{
	while (!shutdown_) {
		std::this_thread::sleep_for(std::chrono::seconds(interval_second));
	}
}

void ezio::add_torrent(std::string torrent_body, std::string save_path)
{
	lt::span<const char> torrent_byte(torrent_body);

	lt::add_torrent_params atp;
	lt::error_code ec;
	const int depth_limit = 100;
	const int token_limit = 10000000;
	const lt::bdecode_node node =
		lt::bdecode(torrent_byte, ec, nullptr, depth_limit, token_limit);

	if (ec) {
		throw std::invalid_argument("failed to decode node");
	}

	atp.ti = std::make_shared<lt::torrent_info>(node, std::ref(ec));
	atp.save_path = save_path;

	if (ec) {
		throw std::invalid_argument("failed to save path");
	}

	lt::torrent_handle handle = session_.add_torrent(std::move(atp));

	// TODO need to setup this via gRPC
	handle.set_max_uploads(3);
	handle.set_max_connections(5);

	SPDLOG_INFO("torrent added. save_path({})", save_path);
}

std::map<std::string, torrent_status> ezio::get_torrent_status(std::vector<std::string> hashes)
{
	std::map<std::string, torrent_status> result;
	// TODO we ignore request hashes first, always return all
	for (auto h : hashes) {
		SPDLOG_DEBUG("hash: {}", h);
		//std::cout << h << std::endl;
		// do some filter for below
	}

	std::vector<lt::torrent_handle> torrents = session_.get_torrents();
	for (lt::torrent_handle const &h : torrents) {
		std::stringstream ss;
		ss << h.info_hash();
		const std::string &hash = ss.str();

		// disable those info we don't need
		lt::torrent_status t_stat = h.status();

		torrent_status status;
		status.hash = hash;
		status.name = t_stat.name;
		status.progress = t_stat.progress;
		status.download_rate = t_stat.download_payload_rate;
		status.upload_rate = t_stat.upload_payload_rate;
		status.active_time = 1;
		status.is_finished = t_stat.is_finished;
		status.num_peers = t_stat.num_peers;
		status.active_time = std::chrono::duration_cast<std::chrono::seconds>(
			t_stat.active_duration)
								 .count();

		result.emplace(hash, status);
	}

	return result;
}

}  // namespace ezio
