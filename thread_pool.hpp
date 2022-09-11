#ifndef __THREAD_POOL_HPP__
#define __THREAD_POOL_HPP__

#include <mutex>
#include <memory>
#include <boost/asio/thread_pool.hpp>
#include <boost/move/utility_core.hpp>
#include <libtorrent/libtorrent.hpp>
#include "buffer_pool.hpp"

namespace ezio
{
struct read_job {
public:
	read_job(char *buffer, buffer_pool *pool,
		std::function<void(libtorrent::disk_buffer_holder,
			libtorrent::storage_error const &)>);

	void operator()();

private:
	char *buffer_;
	buffer_pool *pool_;
	std::function<void(libtorrent::disk_buffer_holder,
		libtorrent::storage_error const &)>
		handler_;
};

struct write_job {
public:
	void operator()();

private:
};

struct hash_job {
public:
	void operator()();
};

class thread_pool
{
public:
	thread_pool();
	~thread_pool() = default;

	void start(int num_threads);
	void stop();
	void submit(const read_job &);
	void submit(read_job &&);
	void submit(const write_job &);
	void submit(write_job &&);
	void submit(const hash_job &);
	void submit(hash_job &&);

private:
	std::mutex mtx_;
	bool started_;
	std::unique_ptr<boost::asio::thread_pool> io_pool_, hash_pool_;
};

}  // namespace ezio

#endif