#include <sstream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <vector>
#include "daemon.hpp"

namespace ezio
{
ezio::ezio(lt::session &session) :
	session_(session), shutdown_(false)
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

		/*
		std::vector<libtorrent::alert*> alerts;
		session_.pop_alerts(&alerts);
		for (auto a : alerts) {
			SPDLOG_INFO("alert: {} {}", a->what(), a->message());
		}
		*/
	}
}

void ezio::add_torrent(std::string torrent_body, std::string save_path, bool seeding_mode = false, int max_uploads = 3, int max_connections = 5)
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
	atp.max_uploads = max_uploads > 0 ? max_uploads : 3;
	atp.max_connections = max_connections > 0 ? max_connections : 5;
	//atp.flags = libtorrent::torrent_flags::default_flags & ~libtorrent::torrent_flags::auto_managed & ~libtorrent::torrent_flags::paused;
	atp.flags = {};

	if (seeding_mode) {
		atp.flags |= libtorrent::torrent_flags::seed_mode;
	}

	if (ec) {
		throw std::invalid_argument("failed to save path");
	}

	lt::torrent_handle handle = session_.add_torrent(std::move(atp));

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

	auto now = std::chrono::high_resolution_clock::now();

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
		status.active_time = std::chrono::duration_cast<std::chrono::seconds>(t_stat.active_duration).count();
		status.state = t_stat.state;
		status.total_done = t_stat.total_done;
		status.total = t_stat.total;
		status.num_pieces = t_stat.num_pieces;
		status.finished_time = std::chrono::duration_cast<std::chrono::seconds>(t_stat.finished_duration).count();
		status.seeding_time = std::chrono::duration_cast<std::chrono::seconds>(t_stat.seeding_duration).count();
		status.total_payload_download = t_stat.total_payload_download;
		status.total_payload_upload = t_stat.total_payload_upload;
		status.is_paused = (t_stat.flags & libtorrent::torrent_flags::paused) != 0;
		status.save_path = t_stat.save_path;

		status.last_upload = -1;
		if (t_stat.last_upload.time_since_epoch().count() != 0) {
			status.last_upload = std::chrono::duration_cast<std::chrono::seconds>(now - t_stat.last_upload).count();
		}

		status.last_download = -1;
		if (t_stat.last_download.time_since_epoch().count() != 0) {
			status.last_download = std::chrono::duration_cast<std::chrono::seconds>(now - t_stat.last_download).count();
		}

		result.emplace(hash, status);
	}

	return result;
}

void ezio::pause_torrent(std::string hash)
{
	SPDLOG_INFO("pause {}", hash);
	std::stringstream ss(hash);
	libtorrent::sha1_hash info_hash;
	ss >> info_hash;
	auto h = session_.find_torrent(info_hash);
	if (h.is_valid()) {
		h.pause();
	}
}

void ezio::resume_torrent(std::string hash)
{
	SPDLOG_INFO("resume {}", hash);
	std::stringstream ss(hash);
	libtorrent::sha1_hash info_hash;
	ss >> info_hash;
	auto h = session_.find_torrent(info_hash);
	if (h.is_valid()) {
		h.resume();
	}
}

bool ezio::get_shutdown()
{
	return shutdown_;
}

void ezio::pop_alerts(std::vector<lt::alert *> *alerts)
{
	session_.pop_alerts(alerts);
}

std::string ezio::get_version()
{
	return GIT_VERSION;
}

}  // namespace ezio
