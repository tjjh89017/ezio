#include <sstream>
#include <chrono>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <vector>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include "daemon.hpp"
#include "version.hpp"

namespace
{
constexpr int SLOW_START_INITIAL = 10 * 1024 * 1024;  // 10 MB/s
constexpr int SLOW_START_STEP = 10 * 1024 * 1024;  // +10 MB/s per period
constexpr int SLOW_START_CAP = 100 * 1024 * 1024;  // 100 MB/s -> beyond = unlimited
}  // namespace

namespace ezio
{
ezio::ezio(lt::session &session, bool slow_start, int slow_start_period) :
	m_session(session),
	m_shutdown(false),
	m_work_guard(boost::asio::make_work_guard(m_ioc)),
	m_reannounce_timer(m_ioc),
	m_signals(m_ioc, SIGINT, SIGTERM),
	m_slow_start_timer(m_ioc),
	m_slow_start(slow_start),
	m_slow_start_period(slow_start_period > 0 ? slow_start_period : 10),
	m_slow_start_limit(0)
{
}

void ezio::stop()
{
	request_shutdown();
}

void ezio::run()
{
	m_signals.async_wait([this](const boost::system::error_code &ec, int /*signum*/) {
		if (ec == boost::asio::error::operation_aborted) {
			return;
		}
		// Re-arm before posting shutdown so a second SIGINT/SIGTERM is not
		// silently swallowed while the shutdown lambda is still pending.
		m_signals.async_wait([this](const boost::system::error_code &ec2, int) {
			if (ec2 != boost::asio::error::operation_aborted) {
				request_shutdown();
			}
		});
		request_shutdown();
	});

	arm_reannounce();

	if (m_slow_start) {
		m_slow_start_limit = SLOW_START_INITIAL;
		apply_session_upload_limit(m_slow_start_limit);
		schedule_slow_start_step();
	}

	m_ioc.run();
}

void ezio::apply_session_upload_limit(int bytes_per_second)
{
	lt::settings_pack p;
	p.set_int(lt::settings_pack::upload_rate_limit, bytes_per_second);
	m_session.apply_settings(p);
	if (bytes_per_second == 0) {
		spdlog::debug("slow-start: upload rate limit cleared (unlimited)");
	} else {
		spdlog::info("slow-start: upload limit set to {} MB/s", bytes_per_second / (1024 * 1024));
	}
}

void ezio::schedule_slow_start_step()
{
	m_slow_start_timer.expires_after(std::chrono::seconds(m_slow_start_period));
	m_slow_start_timer.async_wait([this](const boost::system::error_code &ec) {
		if (ec == boost::asio::error::operation_aborted) {
			return;
		}
		int next = m_slow_start_limit + SLOW_START_STEP;
		if (next >= SLOW_START_CAP) {
			apply_session_upload_limit(0);
			spdlog::info("slow-start complete: upload limit removed");
			return;
		}
		m_slow_start_limit = next;
		apply_session_upload_limit(m_slow_start_limit);
		schedule_slow_start_step();
	});
}

void ezio::arm_reannounce()
{
	m_reannounce_timer.expires_after(std::chrono::seconds(60));
	m_reannounce_timer.async_wait([this](const boost::system::error_code &ec) {
		if (ec == boost::asio::error::operation_aborted) {
			return;
		}
		force_reannounce_all();
		spdlog::debug("periodic force reannounce done");
		arm_reannounce();
	});
}

void ezio::request_shutdown()
{
	// Set eagerly (before the post) so get_shutdown() readers (log, gRPC)
	// see the flag immediately, even though the io_context is still running.
	// Safe cross-thread: m_shutdown is std::atomic_bool.
	m_shutdown = true;
	boost::asio::post(m_ioc, [this] {
		m_reannounce_timer.cancel();
		m_slow_start_timer.cancel();
		m_signals.cancel();
		for (auto &hook : m_shutdown_hooks) {
			hook();
		}
		m_work_guard.reset();
	});
}

lt::io_context &ezio::get_io_context()
{
	return m_ioc;
}

void ezio::register_shutdown_hook(std::function<void()> hook)
{
	m_shutdown_hooks.push_back(std::move(hook));
}

void ezio::add_torrent(std::string torrent_body, std::string save_path, bool seeding_mode = false, int max_uploads = 3, int max_connections = 5, bool sequential_download = false)
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
		// Pre-mark every piece as already verified so libtorrent skips the
		// lazy async_hash that seed_mode normally fires on first peer
		// request. Our seeder's source is the same raw image the torrent
		// metadata was generated from (e.g. partclone -> torrent_create),
		// so re-hashing it on every cold start only catches on-disk
		// corruption we don't try to detect, and costs significant wall
		// time (~48s on a 60 GiB torrent in 1-on-1 tests).
		atp.verified_pieces.resize(atp.ti->num_pieces(), true);
	}

	if (sequential_download) {
		atp.flags |= libtorrent::torrent_flags::sequential_download;
	}

	if (ec) {
		throw std::invalid_argument("failed to save path");
	}

	lt::torrent_handle handle = m_session.add_torrent(std::move(atp));

	spdlog::info("torrent added. save_path({})", save_path);
}

std::map<std::string, torrent_status> ezio::get_torrent_status(std::vector<std::string> hashes)
{
	std::map<std::string, torrent_status> result;
	// TODO we ignore request hashes first, always return all
	for (auto h : hashes) {
		spdlog::debug("hash: {}", h);
		//std::cout << h << std::endl;
		// do some filter for below
	}

	auto now = std::chrono::high_resolution_clock::now();

	std::vector<lt::torrent_handle> torrents = m_session.get_torrents();
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
	spdlog::info("pause {}", hash);
	std::stringstream ss(hash);
	libtorrent::sha1_hash info_hash;
	ss >> info_hash;
	auto h = m_session.find_torrent(info_hash);
	if (h.is_valid()) {
		h.pause();
	}
}

void ezio::resume_torrent(std::string hash)
{
	spdlog::info("resume {}", hash);
	std::stringstream ss(hash);
	libtorrent::sha1_hash info_hash;
	ss >> info_hash;
	auto h = m_session.find_torrent(info_hash);
	if (h.is_valid()) {
		h.resume();
	}
}

bool ezio::get_shutdown()
{
	return m_shutdown;
}

void ezio::pop_alerts(std::vector<lt::alert *> *alerts)
{
	m_session.pop_alerts(alerts);
}

void ezio::set_alert_notify(std::function<void()> const &callback)
{
	m_session.set_alert_notify(callback);
}

void ezio::force_reannounce_all()
{
	for (auto const &h : m_session.get_torrents()) {
		if (h.is_valid()) {
			h.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
		}
	}
}

std::string ezio::get_version()
{
	return EZIO_VERSION;
}

}  // namespace ezio
