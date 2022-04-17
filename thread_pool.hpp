#ifndef __THREAD_POOL_HPP__
#define __THREAD_POOL_HPP__

#include <mutex>
#include <memory>
#include <boost/asio/thread_pool.hpp>
#include <boost/move/utility_core.hpp>
//#include <channel.hpp>

namespace ezio {

struct io_job {
public:
  io_job(const char *buffer);

  void operator()();

private:
  const char *buffer_;
};

struct hash_job {
public:
  void operator()();
};

class thread_pool {
public:
  static thread_pool *get_instance();

  void start(int num_threads);
  void stop();
  void submit(const io_job &);
  void submit(io_job &&);
  void submit(const hash_job &);
  void submit(hash_job &&);

private:
  thread_pool();
  ~thread_pool() = default;

  thread_pool(const thread_pool &) = delete;
  thread_pool(thread_pool &&) = delete;
  thread_pool &operator=(const thread_pool &) = delete;
  thread_pool &operator=(thread_pool &&) = delete;

  std::mutex mtx_;
  bool started_;
  std::unique_ptr<boost::asio::thread_pool> io_pool_, hash_pool_;
};

} // namespace ezio

#endif
