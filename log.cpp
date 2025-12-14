#include <spdlog/spdlog.h>
#include <vector>
#include <libtorrent/libtorrent.hpp>
#include "log.hpp"

namespace ezio
{
log::log(ezio &daemon) :
	m_daemon(daemon)
{
	m_speed = std::thread(&log::report_speed, this);
	m_alert = std::thread(&log::report_alert, this);

	// Setup alert notification callback
	m_daemon.set_alert_notify([this]() {
		// This callback is called from libtorrent internal thread
		// MUST be fast, MUST NOT block, MUST NOT call pop_alerts()
		{
			std::lock_guard<std::mutex> lock(m_alert_mutex);
			m_alert_ready = true;
		}
		m_alert_cv.notify_one();
	});
}

log::~log()
{
	join();
}

void log::join()
{
	m_speed.join();
	m_alert.join();
}

void log::report_speed()
{
	spdlog::info("start speed report thread");
	while (!m_daemon.get_shutdown()) {
		std::this_thread::sleep_for(std::chrono::seconds(5));

		std::vector<std::string> hashes;
		auto result = m_daemon.get_torrent_status(hashes);

		for (const auto &iter : result) {
			const auto &hash = iter.first;
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
	}
}

void log::report_alert()
{
	spdlog::info("start alert report thread");

	while (!m_daemon.get_shutdown()) {
		// Wait for alert notification (with 1-second timeout for shutdown check)
		std::unique_lock<std::mutex> lock(m_alert_mutex);
		m_alert_cv.wait_for(lock, std::chrono::seconds(1), [this]() {
			return m_alert_ready || m_daemon.get_shutdown();
		});

		if (m_daemon.get_shutdown()) {
			break;
		}

		if (!m_alert_ready) {
			// Timeout - check shutdown and continue
			continue;
		}

		m_alert_ready = false;
		lock.unlock();

		// Now pop and process alerts (outside of lock!)
		std::vector<libtorrent::alert *> alerts;
		m_daemon.pop_alerts(&alerts);

		for (auto a : alerts) {
			spdlog::info("lt alert: {} {}", a->what(), a->message());
		}
	}

	spdlog::info("alert report thread exiting");
}

}  // namespace ezio
