#include "thread_pool.hpp"
#include <iostream>
#include <boost/asio/post.hpp>
#include <boost/assert.hpp>
#include <spdlog/spdlog.h>
#include "buffer_pool.hpp"

using namespace std;
using namespace ezio;

read_job::read_job(char *buffer, buffer_pool *pool,
	std::function<void(libtorrent::disk_buffer_holder,
		libtorrent::storage_error const &)>
		handler) :
	buffer_(buffer), pool_(pool), handler_(handler)
{
}

void read_job::operator()()
{
	// do read/write operation.

	lt::storage_error error;
	error.operation = lt::operation_t::file_read;
	error.ec = libtorrent::errors::no_error;

	handler_(libtorrent::disk_buffer_holder(*pool_, buffer_, 123), error);

	SPDLOG_INFO("buffer: {}", buffer_);
}

void write_job::operator()()
{
	SPDLOG_ERROR("write_job is not implemented");
}

void hash_job::operator()()
{
	SPDLOG_ERROR("hash_job is not implemented");
}

thread_pool *thread_pool::get_instance()
{
	static thread_pool inst;
	return &inst;
}

thread_pool::thread_pool()
{
}

void thread_pool::start(int num_threads)
{
	unique_lock<mutex> lk(mtx_);
	if (started_) {
		SPDLOG_ERROR("thread_pool has been started");
		return;
	}

	started_ = true;
	io_pool_ = make_unique<boost::asio::thread_pool>(num_threads);
	hash_pool_ = make_unique<boost::asio::thread_pool>(num_threads);
}

void thread_pool::stop()
{
	unique_lock<mutex> lk(mtx_);
	if (!started_) {
		SPDLOG_ERROR("thread_pool has been stopped");
		return;
	}

	started_ = false;

	io_pool_->join();
	hash_pool_->join();
	io_pool_.reset(nullptr);
	hash_pool_.reset(nullptr);
}

void thread_pool::submit(const read_job &job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(const write_job &job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(const hash_job &job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*hash_pool_, job);
}

void thread_pool::submit(read_job &&job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(write_job &&job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(hash_job &&job)
{
	BOOST_ASSERT(started_);

	boost::asio::post(*hash_pool_, job);
}
