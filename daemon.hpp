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
};

class ezio : boost::noncopyable
{
public:
	ezio(lt::session &);
	~ezio() = default;

	void stop();
	void wait(int interval_second);
	void add_torrent(std::string torrent_body, std::string save_path);
	std::map<std::string, torrent_status> get_torrent_status(std::vector<std::string> hashes);

private:
	lt::session &session_;

	std::atomic_bool shutdown_;
};

}  // namespace ezio

#endif
