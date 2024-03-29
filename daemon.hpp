#ifndef __DAEMON_HPP__
#define __DAEMON_HPP__

#include <atomic>
#include <string>
#include <vector>
#include <libtorrent/libtorrent.hpp>
#include <boost/core/noncopyable.hpp>
#include "service.hpp"

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
	ezio(lt::session &);
	~ezio() = default;

	void stop();
	void wait(int interval_second);
	void add_torrent(std::string torrent_body, std::string save_path, bool seeding_mode, int max_uploads, int max_connections);
	std::map<std::string, torrent_status> get_torrent_status(std::vector<std::string> hashes);
	void pause_torrent(std::string hash);
	void resume_torrent(std::string hash);
	bool get_shutdown();
	void pop_alerts(std::vector<lt::alert *> *);
	std::string get_version();

private:
	lt::session &session_;

	std::atomic_bool shutdown_;
};

}  // namespace ezio

#endif
