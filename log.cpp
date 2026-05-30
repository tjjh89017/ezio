#include <chrono>
#include <vector>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <spdlog/spdlog.h>
#include <libtorrent/libtorrent.hpp>
#include "log.hpp"

namespace ezio
{
log::log(ezio &daemon, lt::io_context &ioc) :
	m_daemon(daemon),
	m_ioc(ioc),
	m_speed_timer(ioc)
{
}

void log::start()
{
	schedule_speed_report();

	m_daemon.set_alert_notify([this] {
		boost::asio::post(m_ioc, [this] {
			process_alerts();
		});
	});

	m_daemon.register_shutdown_hook([this] {
		shutdown();
	});
}

void log::schedule_speed_report()
{
	m_speed_timer.expires_after(std::chrono::seconds(5));
	m_speed_timer.async_wait([this](boost::system::error_code ec) {
		if (ec == boost::asio::error::operation_aborted) {
			return;
		}

		std::vector<std::string> hashes;
		auto result = m_daemon.get_torrent_status(hashes);

		for (const auto &iter : result) {
			const auto &t_stat = iter.second;

			spdlog::info("[{}][{}%][D: {:.2f}MB/s][U: {:.2f}MB/s][{}{}][A: {}][F: {}][S: {}]",
				t_stat.save_path,
				int(t_stat.progress * 100),
				(double)t_stat.download_rate / 1024 / 1024,
				(double)t_stat.upload_rate / 1024 / 1024,
				t_stat.is_paused ? "P" : " ",
				t_stat.is_finished ? "F" : " ",
				t_stat.active_time,
				t_stat.finished_time,
				t_stat.seeding_time);
		}

		schedule_speed_report();
	});
}

void log::process_alerts()
{
	std::vector<libtorrent::alert *> alerts;
	m_daemon.pop_alerts(&alerts);

	for (auto a : alerts) {
		spdlog::info("lt alert: {} {}", a->what(), a->message());
	}
}

void log::shutdown()
{
	m_speed_timer.cancel();
	m_daemon.set_alert_notify({});
}

}  // namespace ezio
