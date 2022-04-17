#include "thread_pool.hpp"
#include <iostream>
#include <boost/asio/post.hpp>
#include <boost/assert.hpp>

using namespace std;
using namespace ezio;

io_job::io_job(const char *buffer) : buffer_(buffer)
{}

void io_job::operator()()
{
  cout << "buffer: " << buffer_ << endl;
}

void hash_job::operator()()
{}

thread_pool *thread_pool::get_instance()
{
  static thread_pool inst;
  return &inst;
}

thread_pool::thread_pool()
{}

void thread_pool::start(int num_threads)
{
  unique_lock<mutex> lk(mtx_);
  if(started_) {
    // SPDLOG_ERROR("thread_pool has been started");
    return;
  }

  started_ = true;
  io_pool_ = make_unique<boost::asio::thread_pool>(num_threads);
  hash_pool_ = make_unique<boost::asio::thread_pool>(num_threads);
}

void thread_pool::stop()
{
  unique_lock<mutex> lk(mtx_);
  if(!started_) {
    // SPDLOG_ERROR("thread_pool has been stopped");
    return;
  }

  started_ = false;

  io_pool_->join();
  hash_pool_->join();
  io_pool_.reset(nullptr);
  hash_pool_.reset(nullptr);
}

void thread_pool::submit(const io_job &job)
{
  BOOST_ASSERT(started_);

  boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(const hash_job &job)
{
  BOOST_ASSERT(started_);

  boost::asio::post(*hash_pool_, job);
}

void thread_pool::submit(io_job &&job)
{
  BOOST_ASSERT(started_);

  boost::asio::post(*io_pool_, job);
}

void thread_pool::submit(hash_job &&job)
{
  BOOST_ASSERT(started_);

  boost::asio::post(*hash_pool_, job);
}
