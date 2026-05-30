#ifndef __DAEMON_HPP__
#define __DAEMON_HPP__

#include <atomic>
#include <csignal>
#include <functional>
#include <string>
#include <vector>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/core/noncopyable.hpp>
#include <libtorrent/libtorrent.hpp>

namespace ezio
{
struct torrent_status {
	std::string hash;
	std::string name;
	double progress;
	int64_t download_rate;
	int64_t upload_rate;
	int64_t active_time;
	bool is_finished;
	int64_t num_peers;
	int64_t state;
	int64_t total_done;
	int64_t total;
	int64_t num_pieces;
	int64_t finished_time;
	int64_t seeding_time;
	int64_t total_payload_download;
	int64_t total_payload_upload;
	bool is_paused;
	std::string save_path;
	int64_t last_upload;
	int64_t last_download;
};

class ezio : boost::noncopyable
{
public:
	ezio(lt::session &session, bool slow_start = false, int slow_start_period = 10);
	~ezio() = default;

	void stop();
	void run();
	void request_shutdown();
	void add_torrent(std::string torrent_body, std::string save_path, bool seeding_mode, int max_uploads, int max_connections, bool sequential_download);
	std::map<std::string, torrent_status> get_torrent_status(std::vector<std::string> hashes);
	void pause_torrent(std::string hash);
	void resume_torrent(std::string hash);
	bool get_shutdown();
	void pop_alerts(std::vector<lt::alert *> *);
	void set_alert_notify(std::function<void()> const &callback);
	void force_reannounce_all();
	std::string get_version();
	lt::io_context &get_io_context();
	void register_shutdown_hook(std::function<void()> hook);

private:
	void arm_reannounce();
	void apply_session_upload_limit(int bytes_per_second);
	void schedule_slow_start_step();

	lt::session &m_session;
	std::atomic_bool m_shutdown;
	lt::io_context m_ioc;
	boost::asio::executor_work_guard<lt::io_context::executor_type> m_work_guard;
	boost::asio::steady_timer m_reannounce_timer;
	boost::asio::signal_set m_signals;
	boost::asio::steady_timer m_slow_start_timer;
	std::vector<std::function<void()>> m_shutdown_hooks;
	bool m_slow_start;
	int m_slow_start_period;
	int m_slow_start_limit;
};

}  // namespace ezio

#endif
